/****************************************************************************
 * ai_engine.c — AI 综合建议模块
 *
 * 整合 ai_chat、ai_image、ai_voice 的结果
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>

#include "ai_engine.h"
#include "../ai_common.h"
#include "../ai_chat/ai_chat.h"
#include "../ai_image/ai_image.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_initialized = false;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_engine_init(void)
{
  if (g_initialized)
    {
      return 0;
    }

  if (!ai_common_is_ready())
    {
      printf("[AI-Engine] Error: AI common not initialized\n");
      return -1;
    }

  g_initialized = true;
  printf("[AI-Engine] Initialized\n");
  return 0;
}

void ai_engine_deinit(void)
{
  g_initialized = false;
}

int ai_engine_run(struct soil_data_s *data,
                  uint8_t *jpeg_data, size_t jpeg_len,
                  struct ai_engine_result_s *result)
{
  if (!g_initialized || !result)
    {
      return -1;
    }

  memset(result, 0, sizeof(struct ai_engine_result_s));

  /* 文本分析 */

  if (data)
    {
      printf("[AI-Engine] Running text analysis...\n");

      if (ai_chat_init() == 0)
        {
          ai_chat_analyze(data, &result->chat);
          ai_chat_deinit();
        }
    }

  /* 图像分析 */

  if (jpeg_data && jpeg_len > 0)
    {
      printf("[AI-Engine] Running image analysis...\n");

      if (ai_image_init() == 0)
        {
          ai_image_analyze(jpeg_data, jpeg_len, &result->image);
          ai_image_deinit();
        }
    }

  /* 生成综合建议 */

  snprintf(result->summary, sizeof(result->summary),
           "综合建议：浇水%s，施肥%s",
           result->chat.water.need ? "需要" : "不需要",
           result->chat.fertilize.need ? "需要" : "不需要");

  return 0;
}

void ai_engine_print_result(const struct ai_engine_result_s *result)
{
  if (!result)
    {
      return;
    }

  printf("[AI-Engine] ========== 综合分析结果 ==========\n");

  /* 文本分析结果 */

  printf("[AI-Engine] 浇水: %s (优先级: %s)\n",
         result->chat.water.need ? "需要" : "不需要",
         result->chat.water.priority);
  printf("[AI-Engine] 原因: %s\n", result->chat.water.reason);

  printf("[AI-Engine] 施肥: %s (优先级: %s)\n",
         result->chat.fertilize.need ? "需要" : "不需要",
         result->chat.fertilize.priority);
  printf("[AI-Engine] 原因: %s\n", result->chat.fertilize.reason);

  /* 图像分析结果（如果有） */

  if (strlen(result->image.health) > 0 &&
      strcmp(result->image.health, "unknown") != 0)
    {
      printf("[AI-Engine] 植物状态: %s\n", result->image.health);
      printf("[AI-Engine] 病害: %s\n", result->image.disease);
      printf("[AI-Engine] 置信度: %.0f%%\n",
             result->image.confidence * 100);
      printf("[AI-Engine] 治疗建议: %s\n", result->image.advice);
    }

  /* 综合建议 */

  printf("[AI-Engine] 综合建议: %s\n", result->summary);
  printf("[AI-Engine] ===================================\n");
}
