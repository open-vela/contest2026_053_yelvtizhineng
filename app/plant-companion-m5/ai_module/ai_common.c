/****************************************************************************
 * ai_common.c — AI 模块公共实现（简化版）
 *
 * 含 ai_common_http_post：裸 socket HTTP POST（参考 ota.c 已验证模式）
 * HTTPS/TLS 为 TODO（需启用 mbedtls/wolfssl + 证书）。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "ai_common.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AI_HTTP_TIMEOUT_MS  15000
#define AI_HTTP_PORT        80        /* 明文调试；HTTPS=443 为 TODO */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_initialized = false;
static char g_api_key[128] = {0};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_common_init(const char *api_key)
{
  if (g_initialized)
    {
      return 0;
    }

  if (!api_key || strlen(api_key) == 0)
    {
      printf("[AI] Error: API key required\n");
      return -1;
    }

  strncpy(g_api_key, api_key, sizeof(g_api_key) - 1);
  g_initialized = true;

  printf("[AI] Initialized (host: api.xiaomimimo.com)\n");
  return 0;
}

void ai_common_deinit(void)
{
  memset(g_api_key, 0, sizeof(g_api_key));
  g_initialized = false;
}

bool ai_common_is_ready(void)
{
  return g_initialized && (strlen(g_api_key) > 0);
}

const char *ai_common_get_api_key(void)
{
  return g_api_key;
}

/****************************************************************************
 * Name: ai_common_http_post
 *
 * 裸 socket HTTP POST（参考 ota.c 已验证的 ota_http_get_json 模式）。
 * 当前 AI_MIMO_HOST 为域名，需 DNS 解析；明文 80 端口调试用。
 * TODO: HTTPS(443) 需 TLS 握手（启用 mbedtls/wolfssl + 证书）。
 ****************************************************************************/

int ai_common_http_post(const char *path, const char *body,
                        char *resp, size_t resp_size)
{
  struct sockaddr_in addr;
  struct timeval tv;
  char request[4096];
  struct hostent *host;
  int fd;
  int ret;
  int total = 0;

  if (!g_initialized || !path || !body || !resp || resp_size == 0)
    {
      return -EINVAL;
    }

  host = gethostbyname(AI_MIMO_HOST);
  if (host == NULL)
    {
      printf("[AI] DNS lookup failed: %s\n", AI_MIMO_HOST);
      return -ENOENT;
    }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  tv.tv_sec  = AI_HTTP_TIMEOUT_MS / 1000;
  tv.tv_usec = (AI_HTTP_TIMEOUT_MS % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(AI_HTTP_PORT);
  memcpy(&addr.sin_addr, host->h_addr, host->h_length);

  ret = connect(fd, (FAR struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0)
    {
      close(fd);
      return -errno;
    }

  /* POST JSON，Authorization: Bearer */

  snprintf(request, sizeof(request),
           "POST %s HTTP/1.1\r\n"
           "Host: %s\r\n"
           "Authorization: Bearer %s\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: %zu\r\n"
           "Connection: close\r\n"
           "\r\n"
           "%s",
           path, AI_MIMO_HOST, g_api_key, strlen(body), body);

  ret = send(fd, request, strlen(request), 0);
  if (ret < 0)
    {
      close(fd);
      return -errno;
    }

  while (total < (int)resp_size - 1)
    {
      ret = recv(fd, resp + total, resp_size - total - 1, 0);
      if (ret <= 0)
        {
          break;
        }

      total += ret;
    }

  resp[total] = '\0';
  close(fd);

  return total > 0 ? total : -EIO;
}
