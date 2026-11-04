/****************************************************************************
 * ai_chat.c — AI 文本分析模块
 *
 * 输入：土壤传感器8参数
 * 输出：浇水/施肥建议
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ai_chat.h"
#include "../ai_common.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define JSON_BUF_SIZE  1024

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_initialized = false;

/* 系统提示 */

static const char *SYSTEM_PROMPT =
  "你是植小伴AI。输入土壤传感器8个参数。"
  "返回JSON：{\"water\":{\"need\":true/false,\"priority\":\"high/medium/low\",\"reason\":\"原因\"},"
  "\"fertilize\":{\"need\":true/false,\"priority\":\"high/medium/low\",\"reason\":\"原因\"}}";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int build_sensor_json(const struct soil_data_s *data,
                             char *json, size_t json_size)
{
  return snprintf(json, json_size,
    "{\"temp\":%.1f,\"moisture\":%.1f,\"ec\":%.1f,"
    "\"salt\":%.1f,\"nitrogen\":%.1f,\"phosphorus\":%.1f,"
    "\"potassium\":%.1f,\"ph\":%.1f}",
    data->temp, data->moisture, data->ec,
    data->salt, data->nitrogen, data->phosphorus,
    data->potassium, data->ph);
}

static int build_request_json(const char *system_prompt,
                              const char *user_message,
                              char *json, size_t json_size)
{
  return snprintf(json, json_size,
    "{\"model\":\"%s\","
    "\"messages\":["
    "{\"role\":\"system\",\"content\":\"%s\"},"
    "{\"role\":\"user\",\"content\":\"%s\"}"
    "],\"max_tokens\":256,\"temperature\":0}",
    AI_MIMO_MODEL, system_prompt, user_message);
}

static bool parse_bool_field(const char *json, const char *field)
{
  char pattern[64];
  char *pos;

  snprintf(pattern, sizeof(pattern), "\"%s\":", field);
  pos = strstr(json, pattern);
  if (!pos)
    {
      return false;
    }

  pos += strlen(pattern);
  return strncmp(pos, "true", 4) == 0;
}

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

static void parse_advice(const char *json, const char *field,
                         struct ai_advice_s *advice)
{
  char pattern[32];
  const char *start;

  snprintf(pattern, sizeof(pattern), "\"%s\":", field);
  start = strstr(json, pattern);
  if (start)
    {
      advice->need = parse_bool_field(start, "need");
      parse_string_field(start, "priority",
                         advice->priority, sizeof(advice->priority));
      parse_string_field(start, "reason",
                         advice->reason, sizeof(advice->reason));
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_chat_init(void)
{
  if (g_initialized)
    {
      return 0;
    }

  if (!ai_common_is_ready())
    {
      printf("[AI-Chat] Error: AI common not initialized\n");
      return -1;
    }

  g_initialized = true;
  printf("[AI-Chat] Initialized\n");
  return 0;
}

void ai_chat_deinit(void)
{
  g_initialized = false;
}

int ai_chat_analyze(const struct soil_data_s *data,
                    struct ai_chat_result_s *result)
{
  if (!g_initialized || !data || !result)
    {
      return -1;
    }

  /* 构建用户消息 */

  char user_msg[512];
  build_sensor_json(data, user_msg, sizeof(user_msg));

  /* 构建请求 JSON */

  char request_json[JSON_BUF_SIZE];
  build_request_json(SYSTEM_PROMPT, user_msg,
                     request_json, sizeof(request_json));

  /* 真实链路：POST MiMo（裸 socket 明文；HTTPS/TLS 为 TODO） */

  {
    char resp[4096];
    int ret;

    ret = ai_common_http_post(AI_MIMO_PATH, request_json, resp, sizeof(resp));
    if (ret > 0)
      {
        memset(result, 0, sizeof(struct ai_chat_result_s));
        parse_advice(resp, "water", &result->water);
        parse_advice(resp, "fertilize", &result->fertilize);

        /* 兜底：响应里无预期字段时记录原始文本 */

        if (!result->water.need && !result->fertilize.need &&
            resp[0] != '\0' && strstr(resp, "\"need\"") == NULL)
          {
            strlcpy(result->water.reason, resp, sizeof(result->water.reason));
          }

        printf("[AI-Chat] Real API response (%d bytes)\n", ret);
        return 0;
      }

    printf("[AI-Chat] HTTP POST failed: %d, fallback to mock\n", ret);
  }

  /* 模拟响应（网络失败时兜底） */

  {
    const char *mock_response =
      "{\"water\":{\"need\":true,\"priority\":\"high\","
      "\"reason\":\"湿度30%低于理想范围\"},"
      "\"fertilize\":{\"need\":false,\"priority\":\"low\","
      "\"reason\":\"EC值正常\"}}";

    memset(result, 0, sizeof(struct ai_chat_result_s));
    parse_advice(mock_response, "water", &result->water);
    parse_advice(mock_response, "fertilize", &result->fertilize);
  }

  return 0;
}

void ai_chat_print_result(const struct ai_chat_result_s *result)
{
  if (!result)
    {
      return;
    }

  printf("[AI-Chat] ========== 文本分析结果 ==========\n");
  printf("[AI-Chat] 浇水: %s (优先级: %s)\n",
         result->water.need ? "需要" : "不需要",
         result->water.priority);
  printf("[AI-Chat] 原因: %s\n", result->water.reason);
  printf("[AI-Chat] 施肥: %s (优先级: %s)\n",
         result->fertilize.need ? "需要" : "不需要",
         result->fertilize.priority);
  printf("[AI-Chat] 原因: %s\n", result->fertilize.reason);
  printf("[AI-Chat] ===================================\n");
}
