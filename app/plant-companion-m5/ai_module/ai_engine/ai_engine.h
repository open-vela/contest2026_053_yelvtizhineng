/****************************************************************************
 * ai_engine.h — AI 综合建议模块
 *
 * 整合 ai_chat、ai_image、ai_voice 的结果
 ****************************************************************************/

#ifndef __AI_ENGINE_H
#define __AI_ENGINE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stddef.h>
#include <stdbool.h>

#include "../ai_chat/ai_chat.h"
#include "../ai_image/ai_image.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* AI 综合结果 */

struct ai_engine_result_s
{
  struct ai_chat_result_s chat;    /* 文本分析结果 */
  struct ai_image_result_s image;  /* 图像分析结果 */
  char summary[256];               /* 综合建议 */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ai_engine_init(void);
void ai_engine_deinit(void);
int ai_engine_run(struct soil_data_s *data,
                  uint8_t *jpeg_data, size_t jpeg_len,
                  struct ai_engine_result_s *result);
void ai_engine_print_result(const struct ai_engine_result_s *result);

#endif /* __AI_ENGINE_H */
