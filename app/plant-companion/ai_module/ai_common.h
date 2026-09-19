/****************************************************************************
 * ai_common.h — AI 模块公共定义（简化版）
 ****************************************************************************/

#ifndef __AI_COMMON_H
#define __AI_COMMON_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MiMo Token Plan API 配置 */

#define AI_MIMO_HOST        "token-plan-cn.xiaomimimo.com"
#define AI_MIMO_PORT        "443"
#define AI_MIMO_PATH        "/v1/chat/completions"
#define AI_MIMO_MODEL       "mimo-v2.5"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ai_common_init(const char *api_key);
void ai_common_deinit(void);
bool ai_common_is_ready(void);
const char *ai_common_get_api_key(void);

/**
 * HTTP POST JSON 到 MiMo API（裸 socket，参考 ota.c 的已验证模式）
 *
 * @param path      API 路径（如 /v1/chat/completions）
 * @param body      JSON 请求体
 * @param resp      响应缓冲区
 * @param resp_size 响应缓冲区大小
 * @return 响应长度（>0）；负 errno 失败
 *
 * 注意：当前走 HTTP 明文（80/8080 调试用）。HTTPS(443) 需启用
 * mbedtls/wolfssl + TLS 握手，见 TODO。真实部署时在此加 TLS。
 */
int ai_common_http_post(const char *path, const char *body,
                        char *resp, size_t resp_size);

#endif /* __AI_COMMON_H */
