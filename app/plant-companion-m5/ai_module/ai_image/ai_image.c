/****************************************************************************
 * ai_image.c — AI 图像分析模块
 *
 * 输入：植物图片（JPEG）
 * 输出：病害诊断
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ai_image.h"
#include "../ai_common.h"

#ifdef CONFIG_CODECS_BASE64
#  include <netutils/base64.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define JSON_BUF_SIZE  8192

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_initialized = false;

/* 系统提示：图像分析 */

static const char *SYSTEM_PROMPT =
  "你是植物病虫害识别专家。分析图片中的植物状态。"
  "返回JSON：{\"health\":\"healthy/sick/critical\",\"disease\":\"病害名称\","
  "\"confidence\":0.9,\"advice\":\"治疗建议\"}";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int parse_string_field(const char *json, const char *field,
                              char *output, size_t output_size)
{
  char pattern[64];
  char *start;
  char *end;

  snprintf(pattern, sizeof(pattern), "\"%s\":\"", field);
  start = strstr(json, pattern);
  if (!start)
    {
      return -1;
    }

  start += strlen(pattern);
  end = strchr(start, '"');
  if (!end)
    {
      return -1;
    }

  size_t len = end - start;
  if (len >= output_size)
    {
      len = output_size - 1;
    }

  memcpy(output, start, len);
  output[len] = '\0';

  return 0;
}

static float parse_float_field(const char *json, const char *field)
{
  char pattern[64];
  char *pos;

  snprintf(pattern, sizeof(pattern), "\"%s\":", field);
  pos = strstr(json, pattern);
  if (!pos)
    {
      return 0.0f;
    }

  pos += strlen(pattern);
  return atof(pos);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_image_init(void)
{
  if (g_initialized)
    {
      return 0;
    }

  if (!ai_common_is_ready())
    {
      printf("[AI-Image] Error: AI common not initialized\n");
      return -1;
    }

  g_initialized = true;
  printf("[AI-Image] Initialized\n");
  return 0;
}

void ai_image_deinit(void)
{
  g_initialized = false;
}

int ai_image_analyze(const uint8_t *jpeg_data, size_t jpeg_len,
                     struct ai_image_result_s *result)
{
  if (!g_initialized || !jpeg_data || !result)
    {
      return -1;
    }

  printf("[AI-Image] Image size: %zu bytes\n", jpeg_len);

#ifdef CONFIG_CODECS_BASE64
  /* 真实链路：Base64 编码 JPEG → POST MiMo（裸 socket，明文调试；
   * HTTPS/TLS 为 TODO，见 ai_common_http_post） */

  {
    size_t b64_len = base64_encode_length(jpeg_len);
    char *b64 = (char *)malloc(b64_len + 1);
    char *body;
    char *resp;
    size_t encoded = 0;
    size_t body_len;
    int ret;

    if (b64 != NULL &&
        base64_encode(jpeg_data, jpeg_len, b64, &encoded) != NULL)
      {
        b64[encoded] = '\0';

        /* 构建请求体：system prompt + image_url(base64) + text */

        body_len = strlen(SYSTEM_PROMPT) + encoded + 512;
        body = (char *)malloc(body_len);
        resp = (char *)malloc(JSON_BUF_SIZE);

        if (body != NULL && resp != NULL)
          {
            snprintf(body, body_len,
                     "{\"model\":\"%s\","
                     "\"messages\":["
                     "{\"role\":\"system\",\"content\":\"%s\"},"
                     "{\"role\":\"user\",\"content\":["
                     "{\"type\":\"image_url\",\"image_url\":{\"url\":"
                     "\"data:image/jpeg;base64,%s\"}},"
                     "{\"type\":\"text\",\"text\":\"请分析这张植物图片的健康状态\"}"
                     "]}],\"max_tokens\":512}",
                     AI_MIMO_MODEL, SYSTEM_PROMPT, b64);

            printf("[AI-Image] POST %s%s (body %zu bytes)\n",
                   AI_MIMO_HOST, AI_MIMO_PATH, strlen(body));

            ret = ai_common_http_post(AI_MIMO_PATH, body, resp, JSON_BUF_SIZE);

            if (ret > 0)
              {
                /* 解析真实响应（找 JSON 部分） */

                memset(result, 0, sizeof(struct ai_image_result_s));
                parse_string_field(resp, "health",
                                   result->health, sizeof(result->health));
                parse_string_field(resp, "disease",
                                   result->disease, sizeof(result->disease));
                result->confidence =
                  parse_float_field(resp, "confidence");
                parse_string_field(resp, "advice",
                                   result->advice, sizeof(result->advice));

                if (result->health[0] == '\0')
                  {
                    /* 响应里没有预期字段：原样记录到 advice */

                    strlcpy(result->advice, resp, sizeof(result->advice));
                  }

                printf("[AI-Image] Real API response (%d bytes)\n", ret);
                free(body);
                free(resp);
                free(b64);
                return 0;
              }

            printf("[AI-Image] HTTP POST failed: %d, fallback to mock\n", ret);
            free(body);
            free(resp);
          }

        free(b64);
      }
  }
#endif /* CONFIG_CODECS_BASE64 */

  /* 模拟响应（无 base64 支持 / 网络失败时兜底，离线可演示） */

  {
    const char *mock_response =
      "{\"health\":\"sick\",\"disease\":\"叶斑病\","
      "\"confidence\":0.85,\"advice\":\"使用多菌灵喷洒，每周一次\"}";

    memset(result, 0, sizeof(struct ai_image_result_s));
    parse_string_field(mock_response, "health",
                       result->health, sizeof(result->health));
    parse_string_field(mock_response, "disease",
                       result->disease, sizeof(result->disease));
    result->confidence = parse_float_field(mock_response, "confidence");
    parse_string_field(mock_response, "advice",
                       result->advice, sizeof(result->advice));

    printf("[AI-Image] Image analysis completed (mock)\n");
  }

  return 0;
}

void ai_image_print_result(const struct ai_image_result_s *result)
{
  if (!result)
    {
      return;
    }

  printf("[AI-Image] ========== 图像分析结果 ==========\n");
  printf("[AI-Image] 植物状态: %s\n", result->health);
  printf("[AI-Image] 病害: %s\n", result->disease);
  printf("[AI-Image] 置信度: %.0f%%\n", result->confidence * 100);
  printf("[AI-Image] 治疗建议: %s\n", result->advice);
  printf("[AI-Image] ===================================\n");
}
