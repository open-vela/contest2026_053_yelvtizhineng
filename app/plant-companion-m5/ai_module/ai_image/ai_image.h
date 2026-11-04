/****************************************************************************
 * ai_image.h — AI 图像分析模块
 *
 * 输入：植物图片（JPEG）
 * 输出：病害诊断
 ****************************************************************************/

#ifndef __AI_IMAGE_H
#define __AI_IMAGE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 图像分析结果 */

struct ai_image_result_s
{
  char health[16];             /* healthy/sick/critical */
  char disease[64];            /* 病害名称 */
  float confidence;            /* 置信度 */
  char advice[256];            /* 治疗建议 */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ai_image_init(void);
void ai_image_deinit(void);
int ai_image_analyze(const uint8_t *jpeg_data, size_t jpeg_len,
                     struct ai_image_result_s *result);
void ai_image_print_result(const struct ai_image_result_s *result);

#endif /* __AI_IMAGE_H */
