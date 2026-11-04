/****************************************************************************
 * apps/plant-companion/services/plant_state.c
 *
 * 植物档案服务：名字/品种/种植日期/天数/心情/健康分
 *
 * 健康分规则（离线，UI_SPEC ①「健康分 80」）：
 *   满分 100，从 5 项各扣罚分，clamp 到 [0,100]：
 *   水分 <20% 扣 25，<40% 扣 10，>80% 扣 10
 *   温度 <10 或 >35 扣 20，<18 或 >28 扣 8
 *   EC <0.6 或 >2.2 mS/cm 扣 15
 * 心情：健康分 >=80 → 😊，>=60 → 🥱，否则 😢
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "plant_state.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct plant_state_s g_state;
static bool g_inited;

/* 默认档案（首次启动） */

static const struct plant_state_s g_default =
{
  .name             = "小绿绿",
  .species          = "绿萝",
  .plant_date_epoch = 0,     /* 由首次 init 设为今天 */
  .plant_day        = 1,
  .health_score     = 80,
  .mood             = PLANT_MOOD_HAPPY,
  .need_water       = false,
};

/* 持久化路径：优先 /data（littlefs 存储分区），否则回退内存模式 */

#define PLANT_STATE_PATH "/data/plant_state.bin"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void state_load(void)
{
  FILE *fp = fopen(PLANT_STATE_PATH, "rb");
  struct plant_state_s loaded;

  if (fp == NULL)
    {
      return;   /* 无持久化：用默认 */
    }

  if (fread(&loaded, sizeof(loaded), 1, fp) == 1)
    {
      /* 基本校验：名字非空 */

      if (loaded.name[0] != '\0' &&
          loaded.species[0] != '\0')
        {
          g_state = loaded;
        }
    }

  fclose(fp);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int plant_state_init(void)
{
  if (g_inited)
    {
      return 0;
    }

  g_state = g_default;

  /* 种植日期默认 = 今天 */

  if (g_state.plant_date_epoch == 0)
    {
      g_state.plant_date_epoch = (uint32_t)time(NULL);
      g_state.plant_day = 1;
    }

  state_load();

  /* 重算天数 */

  {
    time_t now = time(NULL);
    int days = (int)((now - (time_t)g_state.plant_date_epoch) / 86400);

    g_state.plant_day = (days >= 0) ? (days + 1) : 1;
  }

  g_inited = true;
  return 0;
}

void plant_state_get(struct plant_state_s *out)
{
  if (out != NULL)
    {
      *out = g_state;
    }
}

void plant_state_update_from_sensor(float moisture, float temp, float ec_ms)
{
  int score = 100;

  if (!g_inited)
    {
      plant_state_init();
    }

  /* 水分扣分 */

  if (moisture < 20.0f)
    {
      score -= 25;
      g_state.need_water = true;
    }
  else if (moisture < 40.0f)
    {
      score -= 10;
      g_state.need_water = true;
    }
  else if (moisture > 80.0f)
    {
      score -= 10;
      g_state.need_water = false;
    }
  else
    {
      g_state.need_water = false;
    }

  /* 温度扣分 */

  if (temp < 10.0f || temp > 35.0f)
    {
      score -= 20;
    }
  else if (temp < 18.0f || temp > 28.0f)
    {
      score -= 8;
    }

  /* EC 扣分 */

  if (ec_ms < 0.6f || ec_ms > 2.2f)
    {
      score -= 15;
    }

  if (score < 0)
    {
      score = 0;
    }

  g_state.health_score = score;

  /* 心情 */

  if (score >= 80)
    {
      g_state.mood = PLANT_MOOD_HAPPY;
    }
  else if (score >= 60)
    {
      g_state.mood = PLANT_MOOD_TIRED;
    }
  else
    {
      g_state.mood = PLANT_MOOD_SAD;
    }
}

int plant_state_save(void)
{
  FILE *fp = fopen(PLANT_STATE_PATH, "wb");

  if (fp == NULL)
    {
      /* /data 未挂载：静默仅内存（可降级） */

      return 0;
    }

  fwrite(&g_state, sizeof(g_state), 1, fp);
  fclose(fp);
  return 0;
}
