/****************************************************************************
 * ai_chat.h — AI 文本分析模块
 *
 * 输入：土壤传感器8参数
 * 输出：浇水/施肥建议
 ****************************************************************************/

#ifndef __AI_CHAT_H
#define __AI_CHAT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stddef.h>
#include <stdbool.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 土壤传感器数据（8参数） */

struct soil_data_s
{
  float temp;           /* 土壤温度 (°C) */
  float moisture;       /* 土壤湿度 (%) */
  float ec;             /* 电导率 (μS/cm) */
  float salt;           /* 盐分 */
  float nitrogen;       /* 氮 (mg/kg) */
  float phosphorus;     /* 磷 (mg/kg) */
  float potassium;      /* 钾 (mg/kg) */
  float ph;             /* 酸碱度 */
};

/* 浇水/施肥建议 */

struct ai_advice_s
{
  bool need;                   /* 是否需要 */
  char priority[8];            /* 优先级: high/medium/low */
  char reason[128];            /* 原因 */
};

/* 文本分析结果 */

struct ai_chat_result_s
{
  struct ai_advice_s water;    /* 浇水建议 */
  struct ai_advice_s fertilize; /* 施肥建议 */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ai_chat_init(void);
void ai_chat_deinit(void);
int ai_chat_analyze(const struct soil_data_s *data,
                    struct ai_chat_result_s *result);
void ai_chat_print_result(const struct ai_chat_result_s *result);

#endif /* __AI_CHAT_H */
