/****************************************************************************
 * apps/plant-companion/services/server_bridge.c
 *
 * 服务器大脑客户端（设备侧）：语音/图像/上报/时间 → plant_server（/api/v1）。
 *
 * 协议 v2（与 plant_server app/routers/ 各 *.py 对应）：
 *   register → /api/v1/devices/register（SN→device_token）
 *   voice    → /api/v1/voice/session|upload|finalize|audio（会话制）
 *   image    → /api/v1/media/image + /api/v1/ai/diagnose（两步）
 *   telemetry→ /api/v1/telemetry/put
 *   time     → /api/v1/time
 *   actuator → /api/v1/actuators/capabilities|pending|jobs/{id}/ack
 *              （自动执行层：声明能力→取指令→回执）
 *
 * 鉴权：注册成功后所有请求带 X-Device-Token 头（设备无任何大模型密钥）。
 * 实现：裸 socket 明文 HTTP；小块缓冲，无大块堆分配。
 ****************************************************************************/

#include <nuttx/config.h>
#include "power_service.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <nuttx/mm/iob.h>
#include <nuttx/clock.h>
#include "http_chunked.h"
#include "http_response.h"
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>

#include "server_bridge.h"
#include "sd_write.h"
#ifdef CONFIG_PLANT_CAMERA_GC0308
#include "../components/camera_capture/camera_jpeg.h"
#include <nuttx/clock.h>
#endif

#ifdef CONFIG_PLANT_SD_CARD
#  include "../components/system_monitor/device_cfg.h"
#endif
#include "../ota/ota.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SB_TIMEOUT_MS      5000      /* 上传超时（每块） */
#define SB_FINALIZE_TMO    90000     /* finalize 超时（MiMo 理解+TTS；实测 6-15s）
                                      * ⚠️ 2026-09-16：原来 180s 太长——服务器真
                                      * 出问题时用户要盯着"AI思考中"三分钟。 */
#define SB_UPLOAD_TMO      15000     /* 录音整段上传单次 POST 总预算
                                      * （connect/send/recv 各 ≤15s）。掉线时必须
                                      * 快速失败：上传是录音收尾同步等的一环。 */
#define SB_RESP_MAX        4096      /* finalize 只回 {text,has_audio}，几百字节 */

/* 协议 v2：服务器统一前缀与设备身份（SN 即凭证，服务器幂等返回 token） */

#define SB_API_PREFIX      "/api/v1"
#define SB_DEVICE_SN       "ZXB-DEMO-0003"
#define SB_DEVICE_MODEL    "CHD-ESP32-S3-Box"

/* 运行期设备序列号：SD 卡 plant.cfg 里的 sn= 优先，其次编译期 SB_DEVICE_SN。
 * 2026-09-22：一块板一个号，避免 OTA 升级后把设备号改成固件里写死的那个。 */

static char g_sb_sn[40];

static const char *sb_sn(void)
{
  if (g_sb_sn[0] == '\0')
    {
      bool from_cfg = false;

#ifdef CONFIG_PLANT_SD_CARD
      {
        char v[40] = { 0 };

        if (device_cfg_load_sn(v, sizeof(v)) == 0 && v[0] != '\0')
          {
            strncpy(g_sb_sn, v, sizeof(g_sb_sn) - 1);
            from_cfg = true;
          }
      }
#endif

      if (!from_cfg)
        {
          strncpy(g_sb_sn, SB_DEVICE_SN, sizeof(g_sb_sn) - 1);
        }

      g_sb_sn[sizeof(g_sb_sn) - 1] = '\0';
      printf("[Bridge] 设备序列号 SN=%s（%s）\n", g_sb_sn,
             from_cfg ? "SD 配置" : "编译期默认");
    }

  return g_sb_sn;
}

const char *server_bridge_device_sn(void)
{
  return sb_sn();
}

/* 上报给服务器的固件版本号：直接由 OTA 的版本常量拼出来，别再写死。
 * ⚠️ 旧实现是写死的 "v1.3.0-srv"，导致管理台设备列表里的「固件」字段
 * 永远显示 v1.3.0-srv，跟板上实际跑的版本脱节（OTA 升级完也看不出来）。 */

#define SB_STR2(x)         #x
#define SB_STR(x)          SB_STR2(x)
#define SB_DEVICE_FW       "v" SB_STR(OTA_FW_VERSION_MAJOR) "." \
                               SB_STR(OTA_FW_VERSION_MINOR) "." \
                               SB_STR(OTA_FW_VERSION_PATCH) "-srv"
#define SB_REGISTER_TMO    10000
#define SB_MEDIA_TMO       20000     /* 图片上传（raw565 直传，局域网很快） */
#define SB_DIAG_TMO        90000     /* 服务器诊断（转码+MiMo 视觉） */
#define SB_RESP_MEDIA      2048
#define SB_RESP_BIG        8192      /* diagnose 响应（含 raw_model_text） */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static char     g_sb_host[128] = {0};
static uint16_t g_sb_port = 8000;
static int      g_session = -1;
static int      g_seq = 0;
static volatile int g_sb_epoch;   /* 每次 set_server 递增（时间 worker 检测=配置即抓） */
static char     g_sb_token[192] = {0};   /* 设备凭证（register 后写入） */
static int      g_upload_interval_s;     /* 服务器下发的上报周期(s)，0=未拿到 */
static char     g_sid[64] = {0};         /* 语音会话 id（服务器字符串） */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* ⚠️ 2026-09-16 二修：带上限的 TCP 建连。
 *
 * 旧实现直接用阻塞 connect：WiFi 掉线 / 信号差时 lwIP 会按
 * CONFIG_NET_TCP_MAXSYNRTX 一路重发 SYN（本板 5 次、指数退避），
 * 能把调用线程按住几十秒到几分钟。语音上传每秒一次，IO 线程一被按住，
 * 录音队列立刻填满 → 界面永远停在"聆听"、点按钮也没反应。
 *
 * 做法：非阻塞 connect + poll 限时 + SO_ERROR 取结果；
 * 超时就主动放弃（快速失败），绝不再无限等。
 */

static int sb_connect_limited(int fd, FAR struct sockaddr_in *addr,
                              int timeout_ms)
{
  struct pollfd pfd;
  int flags;
  int ret;
  int cerr;
  socklen_t clen;
  int pres;

  if (timeout_ms <= 0)
    {
      timeout_ms = SB_TIMEOUT_MS;
    }

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    {
      flags = 0;
    }

  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  ret = connect(fd, (FAR struct sockaddr *)addr, sizeof(*addr));
  if (ret < 0 && errno != EINPROGRESS && errno != EALREADY &&
      errno != EISCONN)
    {
      fcntl(fd, F_SETFL, flags);
      return -errno;
    }

  if (ret < 0)
    {
      pfd.fd      = fd;
      pfd.events  = POLLOUT;
      pfd.revents = 0;

      pres = poll(&pfd, 1, timeout_ms);
      if (pres == 0)
        {
          fcntl(fd, F_SETFL, flags);
          return -ETIMEDOUT;
        }

      if (pres < 0)
        {
          ret = -errno;
          fcntl(fd, F_SETFL, flags);
          return ret;
        }

      cerr = 0;
      clen = sizeof(cerr);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &cerr, &clen) < 0)
        {
          ret = -errno;
          fcntl(fd, F_SETFL, flags);
          return ret;
        }

      if (cerr != 0)
        {
          fcntl(fd, F_SETFL, flags);
          return -cerr;
        }
    }

  fcntl(fd, F_SETFL, flags);   /* 还原阻塞，后面按 SO_RCVTIMEO 收发 */
  return 0;
}

/* 裸 socket HTTP POST，host/port 可配，body 原样发送，resp 收响应体
 * （跳过 HTTP 头，返回响应体长度）。 */

/* Bound one audio upload socket below the shared IOB pool.332ABBA evidence. */
static _Atomic int g_upload_txbuf=4096;
int server_bridge_upload_txbuf(int bytes)
{
  if(bytes!=0 && bytes!=4096 && bytes!=8192 && bytes!=16384)return -EINVAL;
  if(bytes)atomic_store(&g_upload_txbuf,bytes);
  int fd=socket(AF_INET,SOCK_STREAM,0),actual=-1,requested=atomic_load(&g_upload_txbuf),rc=-1;
  if(fd>=0)
    {
      socklen_t n=sizeof(actual);rc=setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&requested,sizeof(requested));
      if(rc==0)rc=getsockopt(fd,SOL_SOCKET,SO_SNDBUF,&actual,&n);
      close(fd);
    }
  printf("[NetTx] socket_requested=%d socket_applied=%d rc=%d\n",requested,actual,rc);

  printf("[NetTx] upload_sndbuf=%d iob_all=%u iob_tx=%u\n",
         atomic_load(&g_upload_txbuf),iob_navail(false),iob_navail(true));
  return 0;
}

/* Only the single upload worker borrows this descriptor. A joined upload,
 * new voice session, or prepare releases it. Never share it with other HTTP
 * calls, never pipeline requests, and never replay an uncertain upload. */
static _Atomic bool g_upload_keepalive;
static _Atomic unsigned g_upload_fresh,g_upload_reused;
static int g_upload_fd=-1;

void server_bridge_upload_release(void)
{
  if(g_upload_fd>=0){close(g_upload_fd);g_upload_fd=-1;}
}

int server_bridge_upload_keepalive(int mode)
{
  if(mode < -1 || mode > 1)return -EINVAL;
  if(mode>=0)atomic_store(&g_upload_keepalive,mode!=0);
  printf("[UploadKA] enabled=%u fresh=%u reused=%u\n",
         (unsigned)atomic_load(&g_upload_keepalive),
         atomic_load(&g_upload_fresh),atomic_load(&g_upload_reused));
  return 0;
}

static int sb_http_post(const char *host, uint16_t port,
                        const char *path, const char *content_type,
                        const uint8_t *body, size_t body_len,
                        char *resp, size_t resp_size, int timeout_ms)
{
  uint32_t phase_start = clock_systime_ticks();
  uint32_t dns_done, connect_done, send_done;
  struct sockaddr_in addr;
  struct timeval tv;
  struct hostent *hp;
  char head[1024];
  int fd=-1;
  bool upload=strstr(path,"/voice/upload?")!=NULL;
  bool keepalive=upload && atomic_load(&g_upload_keepalive);
  bool reused=false;
  int hl;
  int ret;

  if (host == NULL || host[0] == '\0')
    {
      return -ENOTCONN;
    }

  hp = gethostbyname(host);
  if (hp == NULL)
    {
      /* 允许 IP 直连：inet_addr 转换 */
      in_addr_t ip = inet_addr(host);

      if (ip == INADDR_NONE)
        {
          return -ENOENT;
        }

      addr.sin_addr.s_addr = ip;
    }
  else
    {
      memcpy(&addr.sin_addr, hp->h_addr, hp->h_length);
    }

  dns_done = clock_systime_ticks();
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);

  /* Retry only a failed handshake, before sending any HTTP bytes. Replaying
   * finalize after a response timeout could duplicate upstream work. */
  if(upload && !keepalive)server_bridge_upload_release();
  if(keepalive && g_upload_fd>=0)
    {
      fd=g_upload_fd;g_upload_fd=-1;
      struct pollfd idle={.fd=fd,.events=POLLIN};
      /* Any pending event means EOF, error, or unexpected response bytes.
       * Drop the idle socket before sending a new request in all such cases. */
      if(poll(&idle,1,0)!=0){close(fd);fd=-1;}
      else reused=true;
    }
  if(fd<0)
  for (int attempt = 0; ; attempt++)
    {
      fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return -errno;
      if(strstr(path,"/voice/upload?")!=NULL)
        {
          int cap=atomic_load(&g_upload_txbuf);
          if(setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&cap,sizeof(cap))<0)
            printf("[NetTx] upload sndbuf request=%d rc=%d\n",cap,-errno);
        }
      tv.tv_sec  = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      int connect_ms = timeout_ms > 10000 ? 10000 : timeout_ms;
      ret = sb_connect_limited(fd, &addr, connect_ms);
      if (ret >= 0) break;
      close(fd);
      printf("[Bridge] POST connect failed rc=%d attempt=%d\n", ret, attempt + 1);
      bool transient = ret == -ETIMEDOUT || ret == -EAGAIN ||
                       ret == -ECONNRESET || ret == -ECONNREFUSED;
      if (attempt != 0 || !transient || strstr(path, "/voice/finalize?") == NULL)
        return ret;
    }

  /* 协议 v2：注册成功后自动附带设备凭证头（无 token 阶段不发送） */

  if(keepalive)
    {
      int one=1;
      /* Headers and PCM are separate bounded writes. Suppress Nagle's wait
       * for an ACK between them on reused sockets. */
      if(!reused && setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))<0)
        {
          int error=-errno;
          /* NuttX without TCPPROTO_OPTIONS returns ENOPROTOOPT here; its
           * supported TCP_NODELAY=1 implementation is itself a no-op. Keep
           * the stack default on this one optional-capability response.
           * Socket/state errors still fail before any HTTP bytes. */
          if(error!=-ENOPROTOOPT){close(fd);return error;}
          printf("[UploadKA] nodelay_optional_rc=%d stack_default=1\n",error);
        }
      if(reused)atomic_fetch_add(&g_upload_reused,1);else atomic_fetch_add(&g_upload_fresh,1);
    }
  connect_done = clock_systime_ticks();
  hl = snprintf(head, sizeof(head),
                "POST %s HTTP/1.1\r\n"
                "Host: %s:%u\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n%s%s%s"
                "Connection: %s\r\n\r\n",
                path, host, (unsigned)port, content_type, body_len,
                g_sb_token[0] ? "X-Device-Token: " : "",
                g_sb_token, g_sb_token[0] ? "\r\n" : "",
                keepalive?"keep-alive":"close");
  if(hl<0 || (size_t)hl>=sizeof(head)){close(fd);return -EMSGSIZE;}

  unsigned tx_iobs_before=iob_navail(false);

  /* 发送头部 + body */
  for(int sent=0;sent<hl;sent+=ret)
    {
      ret=send(fd,head+sent,hl-sent,0);
      if(ret<=0){int error=ret<0?-errno:-EIO;close(fd);return error;}
    }

  {
    size_t sent = 0;
    size_t step = 1024;   /* 分小块发送：单次 send 过大时 lwIP 需一次
                           * 提交大 pbuf（可能落在 PSRAM），WiFi TX 会
                           * 立即 EIO；1KB/次与语音块上传同模式，可靠 */

    while (sent < body_len)
      {
        size_t now = body_len - sent;

        if (now > step)
          {
            now = step;
          }

        ret = send(fd, body + sent, now, 0);
        if (ret <= 0)
          {
            printf("[Bridge] send body %u/%u B ret=%d errno=%d\n",
                   (unsigned)sent, (unsigned)body_len, ret, errno);
            close(fd);
            return (ret < 0) ? -errno : -EIO;
          }

        sent += (size_t)ret;
      }
  }

  /* 收响应：跳过 HTTP 头，body 收进 resp */
  send_done = clock_systime_ticks();
  int status = 0;
  ret = plant_http_read_response(fd, resp, resp_size, &status);
  if(keepalive && ret>=0 && status>=200 && status<300)g_upload_fd=fd;
  else close(fd);
  if (strstr(path, "/voice/upload?") != NULL)
    printf("[Perf][HTTP] upload dns=%u connect=%u send=%u response=%u ms status=%d rc=%d\n",
           (unsigned)((dns_done-phase_start)*1000/TICK_PER_SEC),
           (unsigned)((connect_done-dns_done)*1000/TICK_PER_SEC),
           (unsigned)((send_done-connect_done)*1000/TICK_PER_SEC),
           (unsigned)((clock_systime_ticks()-send_done)*1000/TICK_PER_SEC),
           status, ret);
  if(strstr(path,"/voice/upload?")!=NULL)
    printf("[NetTx] cap=%d bytes=%u iob_before=%u iob_after=%u\n",
           atomic_load(&g_upload_txbuf),(unsigned)body_len,tx_iobs_before,iob_navail(false));
  if (status && (status < 200 || status >= 300))
    printf("[Bridge] HTTP status=%d\n", status);
  if (ret < 0)
    printf("[Bridge] response read failed rc=%d\n", ret);
  if ((strstr(path,"/voice/upload?") || strstr(path,"/voice/finalize?")) && (status<200 || status>=300))
    return ret<0?ret:-EPROTO;
  return ret;
}

/* 裸 socket HTTP GET（小响应，收进内存）：跳过头，body 收进 resp。
 * 阶段 B /time 用（响应 <1KB）。与 sb_http_post 同模式。 */

static int sb_http_get(const char *host, uint16_t port,
                       const char *path, char *resp, size_t resp_size,
                       int timeout_ms)
{
  struct sockaddr_in addr;
  struct timeval tv;
  struct hostent *hp;
  char req[768];
  int fd;
  int ret;

  if (host == NULL || host[0] == '\0')
    {
      return -ENOTCONN;
    }

  hp = gethostbyname(host);
  if (hp == NULL)
    {
      in_addr_t ip = inet_addr(host);

      if (ip == INADDR_NONE)
        {
          return -ENOENT;
        }

      addr.sin_addr.s_addr = ip;
    }
  else
    {
      memcpy(&addr.sin_addr, hp->h_addr, hp->h_length);
    }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);

  ret = sb_connect_limited(fd, &addr, timeout_ms);   /* 有上限的建连 */
  if (ret < 0)
    {
      close(fd);
      return ret;
    }

  int rl;

  rl = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s:%u\r\n%s%s%s"
                "Connection: close\r\n\r\n",
                path, host, (unsigned)port,
                g_sb_token[0] ? "X-Device-Token: " : "",
                g_sb_token, g_sb_token[0] ? "\r\n" : "");
  if(rl<0 || (size_t)rl>=sizeof(req)){close(fd);return -EMSGSIZE;}

  for(int sent=0;sent<rl;sent+=ret)
    {
      ret=send(fd,req+sent,rl-sent,0);
      if(ret<=0){int error=ret<0?-errno:-EIO;close(fd);return error;}
    }

  /* 收响应：跳过 HTTP 头，body 收进 resp */
  int status = 0;
  ret = plant_http_read_response(fd, resp, resp_size, &status);
  close(fd);
  if (status && (status < 200 || status >= 300))
    printf("[Bridge] HTTP status=%d\n", status);
  if (ret < 0)
    printf("[Bridge] response read failed rc=%d\n", ret);
  return ret;
}

/* JSON 字段提取：在 resp 中找 "key":"value"（字符串值），返回去引号后的正文。
 *
 * ⚠️ 2026-09-11 修复：旧版扫描值时遇到 '"' 就当字符串结束，**不认反斜杠转义**。
 * 服务器（FastAPI JSONResponse，ensure_ascii=False）把正文里的引号写成 \"，
 * 而模型答话习惯先转写用户的话再加引号，例如：
 *   {"text":"你刚才说的是：\"帮我搜一下绿萝的养殖攻略\"\n\n绿萝很好养哦！…"}
 * 旧版在第一个 \" 处截断 → 设备只显示 "你刚才说的是：\"（实测 22 字节，而
 * 服务器 resp=778 字节、DB 里正文 92 字节），用户看到的就是"回复显示不全"。
 * 现按 JSON 规则跳过 \" \\ 等转义并还原成字符，\uXXXX 解码为 UTF-8（服务器
 * 若改用 ensure_ascii=True 也不会显示成乱码）。
 * 另：值比 out 缓冲长时安全截断后返回实际长度（旧版直接 -EINVAL → 调用者把
 * 整条回复丢成空串）。
 */

/* \uXXXX → 码点；非法十六进制返回 0xFFFFFFFF */

static unsigned int sb_json_hex4(const char *s)
{
  unsigned int v = 0;
  int i;

  for (i = 0; i < 4; i++)
    {
      char c = s[i];

      if (c >= '0' && c <= '9')
        {
          v = (v << 4) | (unsigned int)(c - '0');
        }
      else if (c >= 'a' && c <= 'f')
        {
          v = (v << 4) | (unsigned int)(c - 'a' + 10);
        }
      else if (c >= 'A' && c <= 'F')
        {
          v = (v << 4) | (unsigned int)(c - 'A' + 10);
        }
      else
        {
          return 0xFFFFFFFFu;
        }
    }

  return v;
}

/* 码点 → UTF-8，追加到 out[n]（out_size 已含结尾 '\0' 预留），返回新长度 */

static size_t sb_json_utf8_put(char *out, size_t n, size_t out_size,
                               unsigned int cp)
{
  if (cp < 0x80)
    {
      if (n + 1 < out_size)
        {
          out[n++] = (char)cp;
        }
    }
  else if (cp < 0x800)
    {
      if (n + 2 < out_size)
        {
          out[n++] = (char)(0xC0 | (cp >> 6));
          out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
  else if (cp < 0x10000)
    {
      if (n + 3 < out_size)
        {
          out[n++] = (char)(0xE0 | (cp >> 12));
          out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
          out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
  else
    {
      if (n + 4 < out_size)
        {
          out[n++] = (char)(0xF0 | (cp >> 18));
          out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
          out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
          out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }

  return n;
}

static int sb_json_get_string(const char *json, const char *key,
                              char *out, size_t out_size)
{
  char pat[64];
  const char *p;
  size_t n = 0;

  if (json == NULL || key == NULL || out == NULL || out_size < 2)
    {
      return -EINVAL;
    }

  snprintf(pat, sizeof(pat), "\"%s\"", key);
  p = strstr(json, pat);
  if (p == NULL)
    {
      return -ENOENT;
    }

  p += strlen(pat);
  while (*p == ' ' || *p == ':' || *p == '\t')
    {
      p++;
    }

  if (*p != '"')
    {
      return -EINVAL;
    }

  p++;

  while (*p != '\0' && *p != '"' && n + 1 < out_size)
    {
      if (*p != '\\')
        {
          out[n++] = *p++;
          continue;
        }

      p++;                              /* 跳过反斜杠，看转义字符 */
      switch (*p)
        {
          case '\0':                    /* 结尾残缺 '\'：就此收尾 */
            goto done;

          case '"':  out[n++] = '"';  p++; break;
          case '\\': out[n++] = '\\'; p++; break;
          case '/':  out[n++] = '/';  p++; break;
          case 'b':  out[n++] = '\b'; p++; break;
          case 'f':  out[n++] = '\f'; p++; break;
          case 'n':  out[n++] = '\n'; p++; break;
          case 'r':  out[n++] = '\r'; p++; break;
          case 't':  out[n++] = '\t'; p++; break;

          case 'u':
            {
              unsigned int cp = sb_json_hex4(p + 1);

              p += 5;
              if (cp == 0xFFFFFFFFu)
                {
                  cp = 0xFFFD;          /* 非法转义 → 替换符 */
                }
              else if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                  /* 代理对（emoji）：高位 + 低位合成一个码点 */
                  unsigned int lo = 0;

                  if (p[0] == '\\' && p[1] == 'u')
                    {
                      lo = sb_json_hex4(p + 2);
                    }

                  if (lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                      cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                      p += 6;
                    }
                  else
                    {
                      cp = 0xFFFD;
                    }
                }
              else if (cp >= 0xD800 && cp <= 0xDFFF)
                {
                  cp = 0xFFFD;
                }

              n = sb_json_utf8_put(out, n, out_size, cp);
            }
            break;

          default:                      /* 未知转义：原样保留该字符 */
            out[n++] = *p++;
            break;
        }
    }

done:
  out[n] = '\0';
  return (int)n;
}


/****************************************************************************
 * 协议 v2 JSON 小工具：作用域/转义感知的最小扫描器（无第三方依赖）。
 * 设计说明：结构化诊断要解析嵌套 result{}、media{}、problems[]、suggestions[]
 * —— 字符串里出现同名 key（如 raw_model_text 内嵌整段 JSON）会误导旧式
 * strstr，故这里全部按“字符串外 token”匹配并支持范围限定。
 ****************************************************************************/

/* 在 [beg,end)（end 可 NULL=到串尾）找 "key" token（跳过字符串字面量内的
 * 同名内容），返回冒号后第一个非空字符；找不到返回 NULL。 */

static const char *sb_find_key_n(const char *beg, const char *end,
                                 const char *key)
{
  const char *p = beg;
  bool in_str = false;

  while (p != NULL && *p != '\0' && (end == NULL || p < end))
    {
      if (in_str)
        {
          if (*p == '\\')
            {
              p++;
            }
          else if (*p == '"')
            {
              in_str = false;
            }

          p++;
          continue;
        }

      if (*p == '"')
        {
          const char *q = p + 1;
          const char *k = key;

          while (*k != '\0' && *q == *k)
            {
              q++;
              k++;
            }

          if (*k == '\0' && *q == '"')
            {
              q++;
              while (*q == ' ' || *q == '\t')
                {
                  q++;
                }

              if (*q == ':')
                {
                  q++;
                  while (*q == ' ' || *q == '\t')
                    {
                      q++;
                    }

                  return q;
                }

              return NULL;
            }

          /* 跳过整段字符串，避免在其中继续找 key */
          while (*q != '\0' && *q != '"')
            {
              if (*q == '\\')
                {
                  q++;
                }

              q++;
            }

          p = q;
          continue;
        }

      p++;
    }

  return NULL;
}

/* 全串版本 */

static const char *sb_find_key(const char *json, const char *key)
{
  return sb_find_key_n(json, NULL, key);
}

/* p 指向 '{' 或 '['：返回与之配对的 '}'/']' 之后位置（含字符串/转义感知）。
 * 用于把 result{}、problems[] 等范围圈定，防 raw_model_text 串内误匹配。 */

static const char *sb_matching_end(const char *p)
{
  char open_c;
  char close_c;
  int depth = 0;
  bool in_str = false;

  if (p == NULL || (*p != '{' && *p != '['))
    {
      return NULL;
    }

  open_c  = *p;
  close_c = (open_c == '{') ? '}' : ']';

  while (*p != '\0')
    {
      char ch = *p;

      if (in_str)
        {
          if (ch == '\\')
            {
              p++;
            }
          else if (ch == '"')
            {
              in_str = false;
            }
        }
      else
        {
          if (ch == '"')
            {
              in_str = true;
            }
          else if (ch == open_c)
            {
              depth++;
            }
          else if (ch == close_c)
            {
              depth--;
              if (depth == 0)
                {
                  return p + 1;
                }
            }
        }

      p++;
    }

  return NULL;
}

/* p 指向 JSON 值：返回该值结束后的位置（字符串/对象/数组/数字） */

static const char *sb_json_value_end(const char *p)
{
  if (p == NULL)
    {
      return NULL;
    }

  if (*p == '"')
    {
      p++;
      while (*p != '\0')
        {
          if (*p == '\\')
            {
              p += 2;
              continue;
            }

          if (*p == '"')
            {
              return p + 1;
            }

          p++;
        }

      return NULL;
    }

  if (*p == '{' || *p == '[')
    {
      return sb_matching_end(p);
    }

  while (*p != '\0' && *p != ',' && *p != '}' && *p != ']')
    {
      p++;
    }

  return p;
}

/* 字符串值解码（处理 \" \\ \n \t \r；中文明文 UTF-8 原样保留） */

static int sb_json_str_at(const char *p, char *out, size_t cap)
{
  size_t n = 0;

  if (p == NULL || *p != '"')
    {
      return -EINVAL;
    }

  p++;
  while (*p != '\0')
    {
      char ch = *p++;

      if (ch == '"')
        {
          out[n] = '\0';
          return (int)n;
        }

      if (ch == '\\' && *p != '\0')
        {
          char e = *p++;

          switch (e)
            {
            case 'n':
              ch = '\n';
              break;
            case 'r':
              ch = '\r';
              break;
            case 't':
              ch = '\t';
              break;
            case 'u':
              /* \uXXXX：服务器 ensure_ascii=False，正常不会出现；粗暴跳过 */
              ch = ' ';
              if (*p != '\0')
                {
                  p += 4;
                }

              break;
            default:
              ch = e;    /* \" \\ \/ */
              break;
            }
        }

      if (n + 1 < cap)
        {
          out[n++] = ch;
        }
      else
        {
          break;
        }
    }

  out[n] = '\0';
  return (int)n;
}

/* 整数值解析 */

static int sb_json_int_at(const char *p, int *out)
{
  char *endp;
  long v;

  if (p == NULL)
    {
      return -EINVAL;
    }

  v = strtol(p, &endp, 10);
  if (endp == p)
    {
      return -EINVAL;
    }

  *out = (int)v;
  return 0;
}

/* problems[] 收集：把每题 title/desc 拼成 "title：desc\n"（空 desc 只留 title）。
 * 最多收 max_n 题；输出始终 NUL 结尾。 */

static void sb_collect_issues(const char *arr, char *out, size_t cap,
                              int max_n)
{
  const char *end;
  const char *pos;
  int n = 0;

  out[0] = '\0';
  if (arr == NULL || *arr != '[')
    {
      return;
    }

  end = sb_matching_end(arr);
  if (end == NULL)
    {
      return;
    }

  pos = arr + 1;
  while (n < max_n)
    {
      char title[160];
      char desc[224];
      const char *tp = sb_find_key_n(pos, end, "title");
      const char *dp;
      const char *tve;
      const char *dve;
      size_t used = strlen(out);

      if (tp == NULL)
        {
          break;
        }

      if (sb_json_str_at(tp, title, sizeof(title)) < 0)
        {
          break;
        }

      tve = sb_json_value_end(tp);
      if (tve == NULL)
        {
          break;
        }

      dp = sb_find_key_n(tve, end, "desc");
      if (dp != NULL)
        {
          sb_json_str_at(dp, desc, sizeof(desc));
          dve = sb_json_value_end(dp);
          pos = (dve != NULL) ? dve : tve;
        }
      else
        {
          desc[0] = '\0';
          pos = tve;
        }

      if (used + 8 < cap)
        {
          int w;

          if (desc[0] != '\0')
            {
              w = snprintf(out + used, cap - used, "%s：%s\n",
                           title, desc);
            }
          else
            {
              w = snprintf(out + used, cap - used, "%s\n", title);
            }

          if (w <= 0)
            {
              break;
            }

          if ((size_t)w >= cap - used)
            {
              break;
            }
        }
      else
        {
          break;
        }

      n++;
    }
}

/* suggestions[] 收集：把每项 key 字符串按行拼接（最多 max_n 条） */

static void sb_collect_lines(const char *arr, const char *key,
                             char *out, size_t cap, int max_n)
{
  const char *end;
  const char *pos;
  int n = 0;

  out[0] = '\0';
  if (arr == NULL || *arr != '[')
    {
      return;
    }

  end = sb_matching_end(arr);
  if (end == NULL)
    {
      return;
    }

  pos = arr + 1;
  while (n < max_n)
    {
      char item[288];
      const char *vp = sb_find_key_n(pos, end, key);
      const char *ve;
      size_t used = strlen(out);

      if (vp == NULL || sb_json_str_at(vp, item, sizeof(item)) < 0)
        {
          break;
        }

      ve = sb_json_value_end(vp);
      if (ve == NULL)
        {
          break;
        }

      pos = ve;
      if (used + 2 < cap)
        {
          int w = snprintf(out + used, cap - used, "%s%s\n",
                           (used == 0) ? "" : "", item);

          if (w <= 0 || (size_t)w >= cap - used)
            {
              break;
            }
        }
      else
        {
          break;
        }

      n++;
    }
}

/* ⚠️ 2026-09-10 根因修复（"没有语音播放 / 界面卡死 / 点了没反应"）：
 * 本板 SD(SDMMC) 写盘会把 FILE* 的用户缓冲直接交给 DMA，驱动断言要求
 * 该缓冲 4 字节对齐（arch/xtensa/src/esp32s3/esp32s3_sdmmc.c
 * esp32s3_dmasendsetup: DEBUGASSERT(buffer & 3 == 0)）。
 * 之前用"基址+偏移"的指针写盘（响应头长度随 content-length 位数变化）
 * → 时有对不齐 → 断言 → 板子静默挂死：不播放 TTS、界面随后也不响应。
 * 统一改为先搬到 4 字节对齐的静态缓冲再 fwrite。 */

/* HTTP GET：跳过头，把响应体分块写文件（流式，不整段进内存）。
 * 返回写入字节数。 */

static int sb_http_get_to_file(const char *host, uint16_t port,
                               const char *path,
                               const char *filepath, int timeout_ms)
{
  struct sockaddr_in addr;
  struct timeval tv;
  struct hostent *hp;
  char req[256];
  static uint8_t buf[4097] __attribute__((aligned(4)));  /* 写盘见 sd_write.h */
  char hdr[512];              /* 响应头累积区（头可能跨 recv 包） */
  int  hdr_len = 0;
  FILE *fp;
  int fd;
  int ret;
  int written = 0;
  bool header_done = false;

  if (host == NULL || host[0] == '\0')
    {
      return -ENOTCONN;
    }

  hp = gethostbyname(host);
  if (hp == NULL)
    {
      in_addr_t ip = inet_addr(host);

      if (ip == INADDR_NONE)
        {
          return -ENOENT;
        }

      addr.sin_addr.s_addr = ip;
    }
  else
    {
      memcpy(&addr.sin_addr, hp->h_addr, hp->h_length);
    }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);

  ret = sb_connect_limited(fd, &addr, timeout_ms);   /* 有上限的建连 */
  if (ret < 0)
    {
      close(fd);
      return ret;
    }

  int rl;

  rl = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s:%u\r\n",
                path, host, (unsigned)port);

  if (g_sb_token[0] != '\0')
    {
      rl += snprintf(req + rl, sizeof(req) - (size_t)rl,
                     "X-Device-Token: %s\r\n", g_sb_token);
    }

  rl += snprintf(req + rl, sizeof(req) - (size_t)rl,
                 "Connection: close\r\n\r\n");

  ret = send(fd, req, rl, 0);
  if (ret < 0)
    {
      close(fd);
      return -errno;
    }

  fp = fopen(filepath, "wb");
  if (fp == NULL)
    {
      close(fd);
      return -errno;
    }

  /* ⚠️ 2026-08-31 增强：校验 HTTP 状态码（此前不检查，404 的 JSON body
   * 会被当音频写盘 → play_file 无 RIFF 头静默失败 → 喇叭无声）。
   * 状态码解析与头跳过/写文件共用同一接收循环，避免吃掉含 RIFF 头
   * 的第一包数据。 */

  {
    int status = 0;
    bool status_done = false;

    for (;;)
      {
        int n = recv(fd, buf, sizeof(buf) - 1, 0);

        if (n <= 0)
          {
            /* ⚠️ 2026-08-31 增强：打印 recv 结束原因（0=对端关闭，
             * 负=errno）。此前静默 break → 服务器端只见 RST，
             * 设备端原因不可见。 */
            if (n < 0)
              {
                printf("[Bridge] GET %s recv 结束 errno=%d "
                       "(已写 %d B)\n", path, errno, written);
              }
            else
              {
                printf("[Bridge] GET %s 对端关闭 (已写 %d B)\n",
                       path, written);
              }

            break;
          }

        buf[n] = '\0';   /* ⚠️ 2026-08-31 修复：recv 原始字节流无 NUL 结尾，
                          * strstr/strncmp 在无终止符缓冲上扫描是未定义行为
                          * → 状态行可能误判 → remove+close → 服务器 BrokenPipe */

        if (!status_done)
          {
            char *sp = strstr((char *)buf, " ");

            if (sp != NULL && (size_t)(sp - (char *)buf) >= 7 &&
                strncmp((char *)buf, "HTTP/1.", 7) == 0)
              {
                status = atoi(sp + 1);
                status_done = true;

                if (status != 200)
                  {
                    /* 非 200：响应体是错误 JSON，丢弃，不写盘 */
                    break;
                  }
              }
            else
              {
                /* 状态行未收全或非 HTTP 响应 → 保守失败。
                 * ⚠️ 打印首包前 16 字节（hex）便于诊断：若开头不是
                 * "HTTP/1."，说明 TCP 分片把状态行拆开了（罕见但存在），
                 * 需要累积缓冲而非单包解析。 */
                printf("[Bridge] GET %s 状态行异常，首包: ",
                       path);
                for (int di = 0; di < n && di < 16; di++)
                  {
                    printf("%02X ", buf[di]);
                  }

                printf("\n");
                break;
              }
          }

        if (!header_done)
          {
            char *p;
            int skip;

            /* ⚠️ 2026-09-10 修复：头可能跨 recv 包（\r\n\r\n 落在包边界）。
             * 旧实现找不到空行就 continue 丢整包 → 丢掉 "\n" + "RIFF"
             * 开头 → 落盘文件不是合法 WAV（无 RIFF 头）→
             * ai_voice_play_file 静默 -EINVAL → 喇叭无声。
             * 改为先攒头，找到空行再把本包剩余 body 写盘。 */

            int prev = hdr_len;
            int take = n;
            int boff;

            if ((size_t)hdr_len + (size_t)take >= sizeof(hdr))
              {
                take = (int)sizeof(hdr) - 1 - hdr_len;
              }

            if (take < 0)
              {
                take = 0;
              }

            memcpy(hdr + hdr_len, buf, (size_t)take);
            hdr_len += take;
            hdr[hdr_len] = '\0';

            p = strstr(hdr, "\r\n\r\n");
            if (p == NULL)
              {
                if (hdr_len >= (int)sizeof(hdr) - 1)
                  {
                    break;   /* 头异常超长，放弃 */
                  }

                continue;
              }

            skip = (int)(p - hdr) + 4;
            header_done = true;

            /* ⚠️ 2026-09-10 修复：旧实现把本包长度 n 截断到 hdr
             * 容量后再从 hdr 里取 body，本包中超出 hdr 容量的
             * body 被整段丢弃（实测每轮固定丢 3585 B，WAV 尾部
             * 被截）。改为直接从本包取 body，下标 = skip - prev。 */

            boff = skip - prev;
            if (boff < 0)
              {
                boff = 0;
              }

            if (boff > n)
              {
                boff = n;
              }

            if (n - boff > 0)
              {
                /* ⚠️ 不能直接 fwrite(buf + body)：body 任意 →
                 * 指针可能对不齐 → SD DMA 断言挂机。 */
                if (sd_write_aligned(fp, buf + boff,
                                     (size_t)(n - boff)) < 0)
                  {
                    printf("[Bridge] GET %s 写盘失败\n", path);
                    break;
                  }

                written += n - boff;
              }

            hdr_len = 0;
            continue;
          }
        else
          {
            if (sd_write_aligned(fp, buf, (size_t)n) < 0)
              {
                printf("[Bridge] GET %s 写盘失败\n", path);
                break;
              }

            written += n;
          }
      }

    if (!status_done || status != 200)
      {
        printf("[Bridge] GET %s -> HTTP %d（已写 %d B，文件保留待查）\n",
               path, status, written);
        fclose(fp);
        close(fd);
        return -EIO;
      }
  }

  fclose(fp);
  close(fd);
  return written;
}

/* 2026-09-16 时延修复：流式 GET（边下边播，不落盘）。
 * 见 server_bridge.h 的说明；这里只负责 HTTP 层：建连、校验 200、
 * 跳过响应头（允许跨包）、把 body 原样交给回调。 */

struct sb_stream_sink {server_bridge_stream_cb_t cb;void *arg;size_t *total,max;};
static int sb_chunk_audio(const uint8_t *p,size_t n,void *arg)
{
  struct sb_stream_sink *s=arg;
  if(s->max && (n>s->max || *s->total>s->max-n))return -EMSGSIZE;
  int rc=s->cb(p,n,s->arg);if(rc<0)return rc;*s->total+=n;return 0;
}
int server_bridge_get_stream(const char *path,
                             server_bridge_stream_cb_t cb, void *arg,
                             int stall_timeout_ms, size_t max_bytes)
{
  struct sockaddr_in addr;
  struct timeval tv;
  struct hostent *hp;
  const char *host = g_sb_host;
  /* ⚠️ 2026-09-16 实测：请求头里 X-Device-Token 是 ~180B 的 JWT，
   * 加上请求行/Host/Accept-Encoding/Connection 一共 ~250B，256 字节的
   * 缓冲会被 snprintf 截断 → 结尾的 

 丢失 → uvicorn 回 400
   * （日志：[Bridge] STREAM ... -> HTTP/1.1 400 Bad Request）。
   * 老函数 sb_http_get_to_file 同为 256B 但没发 Accept-Encoding，刚好够。 */
  char req[512];
  static uint8_t buf[2049] __attribute__((aligned(4)));
  char hdr[384];
  int  hdr_len = 0;
  int fd;
  int rl;
  int ret;
  size_t total = 0;
  unsigned recv_calls=0,max_recv_ms=0,recv_over100=0;
  bool header_done = false;
  bool chunked=false;
  struct plant_chunked chunks={0};
  struct sb_stream_sink sink={cb,arg,&total,max_bytes};

  if (host[0] == '\0' || path == NULL || cb == NULL)
    {
      return -ENOTCONN;
    }

  if (stall_timeout_ms <= 0)
    {
      stall_timeout_ms = 20000;
    }

  hp = gethostbyname(host);
  if (hp == NULL)
    {
      in_addr_t ip = inet_addr(host);

      if (ip == INADDR_NONE)
        {
          return -ENOENT;
        }

      addr.sin_addr.s_addr = ip;
    }
  else
    {
      memcpy(&addr.sin_addr, hp->h_addr, hp->h_length);
    }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  /* 收数据停滞超时（不是总超时）：音频可能播几十秒，期间一直有数据
   * 到达就不该断；连着 stall_timeout_ms 收不到才判定掉线。 */

  tv.tv_sec  = stall_timeout_ms / 1000;
  tv.tv_usec = (stall_timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  addr.sin_family = AF_INET;
  addr.sin_port   = htons(g_sb_port);

  ret = sb_connect_limited(fd, &addr, stall_timeout_ms);   /* 有上限的建连 */
  if (ret < 0)
    {
      close(fd);
      return ret;
    }

  rl = snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s:%u\r\n"
                "Accept-Encoding: identity\r\n",
                path, host, (unsigned)g_sb_port);

  if (g_sb_token[0] != '\0')
    {
      rl += snprintf(req + rl, sizeof(req) - (size_t)rl,
                     "X-Device-Token: %s\r\n", g_sb_token);
    }

  rl += snprintf(req + rl, sizeof(req) - (size_t)rl,
                 "Connection: close\r\n\r\n");

  ret = send(fd, req, rl, 0);
  if (ret < 0)
    {
      close(fd);
      return -errno;
    }

  for (;;)
    {
      uint32_t recv_begin=clock_systime_ticks();
      int n = recv(fd, buf, sizeof(buf) - 1, 0);
      unsigned recv_ms=(unsigned)((clock_systime_ticks()-recv_begin)*1000/TICK_PER_SEC);
      recv_calls++;if(recv_ms>max_recv_ms)max_recv_ms=recv_ms;
      if(recv_ms>=100)recv_over100++;

      if (n <= 0)
        {
          if (n < 0)
            {
              printf("[Bridge] STREAM %s recv 结束 errno=%d (已收 %u B)\n",
                     path, errno, (unsigned)total);
            }

          break;
        }

      buf[n] = '\0';

      if (!header_done)
        {
          char *p;
          int prev = hdr_len;
          int take = n;
          int boff;

          if ((size_t)hdr_len + (size_t)take >= sizeof(hdr))
            {
              take = (int)sizeof(hdr) - 1 - hdr_len;
            }

          if (take < 0)
            {
              take = 0;
            }

          memcpy(hdr + hdr_len, buf, (size_t)take);
          hdr_len += take;
          hdr[hdr_len] = '\0';

          p = strstr(hdr, "\r\n\r\n");
          if (p == NULL)
            {
              if (hdr_len >= (int)sizeof(hdr) - 1)
                {
                  printf("[Bridge] STREAM %s 响应头超长，放弃\n", path);
                  close(fd);
                  return -EIO;
                }

              continue;
            }

          /* 状态行必须是 200：404/502 的 JSON 体不能当音频播 */

          {
            char *sp = strstr(hdr, " ");

            if (sp == NULL || strncmp(hdr, "HTTP/1.", 7) != 0 ||
                atoi(sp + 1) != 200)
              {
                printf("[Bridge] STREAM %s -> %.48s（非 200，放弃）\n",
                       path, hdr);
                close(fd);
                return -EIO;
              }
          }

          chunked=strstr(hdr,"Transfer-Encoding: chunked")!=NULL ||
                  strstr(hdr,"transfer-encoding: chunked")!=NULL;

          header_done = true;
          boff = (int)((size_t)(p - hdr) + 4) - prev;
          hdr_len = 0;

          if (boff < 0)
            {
              boff = 0;
            }

          if (boff > n)
            {
              boff = n;
            }

          if (n - boff > 0)
            {
              if(chunked)
                {ret=plant_chunk_feed(&chunks,buf+boff,(size_t)(n-boff),sb_chunk_audio,&sink);
                 if(ret<0){close(fd);return ret;}
                 if(chunks.state==5)break;
                 continue;}
              if (max_bytes > 0 && (size_t)(n - boff) > max_bytes)
                {
                  printf("[Bridge] STREAM %s 超过上限 %u B，中止\n",
                         path, (unsigned)max_bytes);
                  close(fd);
                  return -EIO;
                }

              ret = cb(buf + boff, (size_t)(n - boff), arg);
              if (ret < 0)
                {
                  printf("[Bridge] STREAM %s 回调中止(%d)，已收 %u B\n",
                         path, ret, (unsigned)total);
                  close(fd);
                  return ret;
                }

              total += (size_t)(n - boff);
            }

          continue;
        }

      if(chunked)
        {ret=plant_chunk_feed(&chunks,buf,(size_t)n,sb_chunk_audio,&sink);
         if(ret<0){close(fd);return ret;}if(chunks.state==5)break;continue;}
      if (max_bytes > 0 && total + (size_t)n > max_bytes)
        {
          printf("[Bridge] STREAM %s 超过上限 %u B，中止\n",
                 path, (unsigned)max_bytes);
          close(fd);
          return -EIO;
        }

      ret = cb(buf, (size_t)n, arg);
      if (ret < 0)
        {
          printf("[Bridge] STREAM %s 回调中止(%d)，已收 %u B\n",
                 path, ret, (unsigned)total);
          close(fd);
          return ret;
        }

      total += (size_t)n;
    }

  close(fd);

  printf("[StreamRX] calls=%u max_wait_ms=%u waits_over100=%u body_bytes=%u\n",
         recv_calls,max_recv_ms,recv_over100,(unsigned)total);
  if(chunked && chunks.state!=5)return -EIO;
  if (!header_done)
    {
      printf("[Bridge] STREAM %s 未收到完整响应头\n", path);
      return -EIO;
    }

  return (int)total;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int server_bridge_set_server(const char *host, uint16_t port)
{
  char resolved[64];

  if (host == NULL || host[0] == '\0' || strlen(host) >= sizeof(g_sb_host))
    {
      return -EINVAL;
    }

  /* Resolve once. Repeating gethostbyname() for every PCM upload added
   * several seconds per request on the CoreS3 resolver. The plain-HTTP
   * server is also reachable through its IPv4 address. */

  strncpy(resolved, host, sizeof(resolved) - 1);
  resolved[sizeof(resolved) - 1] = '\0';

  if (inet_addr(host) == INADDR_NONE)
    {
      struct hostent *hp = gethostbyname(host);

      if (hp != NULL && hp->h_addrtype == AF_INET &&
          hp->h_length == sizeof(struct in_addr))
        {
          if (inet_ntop(AF_INET, hp->h_addr, resolved,
                        sizeof(resolved)) == NULL)
            {
              strncpy(resolved, host, sizeof(resolved) - 1);
              resolved[sizeof(resolved) - 1] = '\0';
            }
        }
    }

  /* 换服务器 = 旧凭证作废，重新注册取 token */

  strncpy(g_sb_host, resolved, sizeof(g_sb_host) - 1);
  g_sb_host[sizeof(g_sb_host) - 1] = '\0';
  g_sb_port = port;
  g_sb_token[0] = '\0';
  g_session = -1;
  g_sid[0] = '\0';
  g_sb_epoch++;   /* 通知时间 worker 立即重新抓取（配置即抓） */
  printf("[Bridge] server = %s (%s):%u\n", host, g_sb_host,
         (unsigned)g_sb_port);
  return server_bridge_device_login();
}

int server_bridge_config_epoch(void)
{
  return g_sb_epoch;
}

const char *server_bridge_host(void)
{
  return g_sb_host;
}

int server_bridge_port(void) { return g_sb_port; }

const char *server_bridge_device_token(void)
{
  return g_sb_token;
}

/* 服务器下发的传感器上报周期（秒）；0 = 还没拿到，调用方用本地默认值 */

int server_bridge_upload_interval_s(void)
{
  return g_upload_interval_s;
}

/* 设备注册：SN 即凭证（演示期）。已注册时服务器幂等返回既有 token。
 * 成功后 g_sb_token 生效，此后所有请求自动带 X-Device-Token。 */

int server_bridge_device_login(void)
{
  char body[192];
  static char resp[2048];
  int ret;

  if (g_sb_host[0] == '\0')
    {
      return -ENOTCONN;
    }

  snprintf(body, sizeof(body),
           "{\"sn\":\"%s\",\"model\":\"%s\",\"fw_version\":\"%s\"}",
           sb_sn(), SB_DEVICE_MODEL, SB_DEVICE_FW);

  ret = sb_http_post(g_sb_host, g_sb_port,
                     SB_API_PREFIX "/devices/register",
                     "application/json",
                     (const uint8_t *)body, strlen(body),
                     resp, sizeof(resp), SB_REGISTER_TMO);
  if (ret < 0)
    {
      printf("[Bridge] 设备注册 HTTP 失败: %d（服务器需为 plant_server）\n", ret);
      return ret;
    }

  g_sb_token[0] = '\0';
  if (sb_json_get_string(resp, "device_token", g_sb_token,
                         sizeof(g_sb_token)) < 0)
    {
      printf("[Bridge] 注册响应无 device_token (%.80s)\n", resp);
      return -EINVAL;
    }

  printf("[Bridge] 设备注册成功 SN=%s token=%.10s…\n",
         sb_sn(), g_sb_token);
  return 0;
}

static char g_voice_action[24],g_voice_arg[64];
static bool g_voice_reply_pending;
bool server_bridge_voice_reply_pending(void) { return g_voice_reply_pending; }
int server_bridge_take_action(char *action,size_t an,char *arg,size_t bn,char *id,size_t cn)
{
  strlcpy(action,g_voice_action,an);strlcpy(arg,g_voice_arg,bn);strlcpy(id,g_sid,cn);
  g_voice_action[0]=0;
  return *action?1:0;
}

int server_bridge_session_begin(void)
{
  server_bridge_upload_release();
  char path[96];
  static char resp[1024];
  int ret;

  if (g_sb_host[0] == '\0')
    {
      return -ENOTCONN;
    }

  g_sid[0] = '\0';
  g_voice_action[0]=0;g_voice_arg[0]=0;
  g_voice_reply_pending=false;

  /* 协议 v2：服务器建会话返回字符串 session_id（如 vs_xxx） */

  snprintf(path, sizeof(path), SB_API_PREFIX "/voice/session");
  ret = sb_http_post(g_sb_host, g_sb_port, path,
                     "application/json",
                     #ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
                     (const uint8_t *)"{\"interaction\":true,\"stream_audio\":true,\"stream_reply\":true,\"power_sleep\":true}",
                     sizeof("{\"interaction\":true,\"stream_audio\":true,\"stream_reply\":true,\"power_sleep\":true}")-1,
#else
                     (const uint8_t *)"{}", 2,
#endif
                     resp, sizeof(resp), SB_TIMEOUT_MS * 2);
  if (ret < 0)
    {
      printf("[Bridge] voice session 创建失败: %d\n", ret);
      return ret;
    }

  if (sb_json_get_string(resp, "session_id", g_sid, sizeof(g_sid)) < 0)
    {
      printf("[Bridge] voice session 响应无 session_id (%.80s)\n", resp);
      return -EINVAL;
    }

  g_session = 1;
  g_seq = 0;
  printf("[Bridge] voice session = %s\n", g_sid);
  return 0;
}

static bool sb_upload_uint_equals(const char *json,const char *key,size_t expected)
{
  const char *p=sb_find_key(json,key);
  size_t value=0;
  if(!p || *p<'0' || *p>'9')return false;
  do
    {
      unsigned digit=(unsigned)(*p++-'0');
      if(value>(SIZE_MAX-digit)/10)return false;
      value=value*10+digit;
    }
  while(*p>='0' && *p<='9');
  while(*p==' ' || *p=='\t' || *p=='\r' || *p=='\n')p++;
  return value==expected && (*p==',' || *p=='}');
}

int server_bridge_upload(const int16_t *pcm, size_t samples)
{
  char path[160];
  char resp[512];
  size_t bytes = samples * 2;
  int ret;

  if (g_session <= 0 || g_sid[0] == '\0' || g_sb_host[0] == '\0')
    {
      return -ENOTCONN;
    }

  snprintf(path, sizeof(path),
           SB_API_PREFIX "/voice/upload?session_id=%s&seq=%d",
           g_sid, g_seq);

  ret = sb_http_post(g_sb_host, g_sb_port, path,
                     "application/octet-stream",
                     (const uint8_t *)pcm, bytes,
                     resp, sizeof(resp), SB_UPLOAD_TMO);
  if (ret < 0)
    {
      server_bridge_upload_release();return ret;
    }

  const char *ok=sb_find_key(resp,"ok");
  if(!ok || strncmp(ok,"true",4) ||
     (ok[4]!=',' && ok[4]!='}' && ok[4]!=' ' && ok[4]!='\t' && ok[4]!='\r' && ok[4]!='\n') ||
     !sb_upload_uint_equals(resp,"seq",(size_t)g_seq) ||
     !sb_upload_uint_equals(resp,"received",bytes))
    {printf("[Bridge] upload acknowledgement mismatch; do not replay\n");server_bridge_upload_release();return -EPROTO;}
  g_seq++;
  return ret;
}

int server_bridge_voice_prepare(bool *thinking,char *audio_path,size_t capacity)
{
  server_bridge_upload_release();
  char path[160],response[512];
  if(!thinking||!audio_path||!capacity)return -EINVAL;
  *thinking=false;audio_path[0]=0;
  if(g_session<=0||!g_sid[0])return -ENOTCONN;
  snprintf(path,sizeof(path),SB_API_PREFIX "/voice/prepare?session_id=%s",g_sid);
  int rc=sb_http_post(g_sb_host,g_sb_port,path,"application/octet-stream",NULL,0,
                      response,sizeof(response),SB_FINALIZE_TMO);
  if(rc<0)return rc;
  const char *p=strstr(response,"\"thinking\"");
  if(!p)return -ENOSYS; /* Older server: use the original finalize path. */
  p+=10;while(*p==' '||*p==':'||*p=='\t')p++;
  *thinking=!strncmp(p,"true",4);
  if(*thinking && (sb_json_get_string(response,"thinking_audio",audio_path,capacity)<0 ||
                  strcmp(audio_path,SB_API_PREFIX "/voice/thinking")))
    {*thinking=false;audio_path[0]=0;return -EINVAL;}
  return 0;
}

static bool g_voice_known_no_speech;
bool server_bridge_voice_known_no_speech(void){return g_voice_known_no_speech;}

int server_bridge_finalize(char *text_out, size_t text_size,
                           bool *has_audio)
{
  char path[160];
  static char resp[SB_RESP_MAX]; /* static: 16KB 栈必溢出 */
  int ret;

  if(!text_out || !text_size)return -EINVAL;
  text_out[0]='\0';
  g_voice_known_no_speech=false;
  if(has_audio)*has_audio=false;
  g_voice_reply_pending=false;
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  g_voice_action[0]='\0';g_voice_arg[0]='\0';
#endif

  if (g_session <= 0 || g_sid[0] == '\0' || g_sb_host[0] == '\0')
    {
      return -ENOTCONN;
    }

  snprintf(path, sizeof(path),
           SB_API_PREFIX "/voice/finalize?session_id=%s", g_sid);

  ret = sb_http_post(g_sb_host, g_sb_port, path,
                     "application/octet-stream",
                     NULL, 0,
                     resp, sizeof(resp), SB_FINALIZE_TMO);
  if (ret < 0)
    {
      return ret;
    }

  const char *audio_flag=sb_find_key(resp,"has_audio");
  if(!audio_flag || !((!strncmp(audio_flag,"true",4) &&
      (audio_flag[4]==',' || audio_flag[4]=='}' || isspace((unsigned char)audio_flag[4]))) ||
      (!strncmp(audio_flag,"false",5) &&
      (audio_flag[5]==',' || audio_flag[5]=='}' || isspace((unsigned char)audio_flag[5])))) ||
      sb_json_get_string(resp,"text",text_out,text_size)<0)
    {text_out[0]='\0';return -EBADMSG;}

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  char activity_text[128]={0};
  if(sb_json_get_string(resp,"said",activity_text,sizeof(activity_text))==0)
    {
      /* Ignore common short ASR noise. Neither UI timers nor failed replies
       * extend the inactivity timer. The raw text is never logged here. */
      size_t n=strlen(activity_text);
      g_voice_known_no_speech=n==0;
      if(n>=6 && strcmp(activity_text,"Yeah.") && strcmp(activity_text,"嗯。") &&
         strcmp(activity_text,"好的") && strcmp(activity_text,"嗯嗯"))power_service_activity();
    }
  sb_json_get_string(resp,"action",g_voice_action,sizeof(g_voice_action));
  sb_json_get_string(resp,"arg",g_voice_arg,sizeof(g_voice_arg));
#endif
  {
    const char *pending=sb_find_key(resp,"reply_stream");
    g_voice_reply_pending=pending && strncmp(pending,"true",4)==0;
  }

  /* 解析 text */
  if (sb_json_get_string(resp, "text", text_out, text_size) < 0)
    {
      text_out[0] = '\0';
    }

  /* 解析 has_audio（⚠️ 2026-08-31 修复：服务器返回的是 JSON 布尔
   * "has_audio": true（无引号），而 sb_json_get_string 只支持字符串值
   * （要求 "key":"..."）→ 对布尔返回 -EINVAL → has_audio 误判为 false
   * → 设备不下载不播放 → 喇叭无声。这里单独做布尔解析。 */
  if (has_audio != NULL)
    {
      const char *q = strstr(resp, "\"has_audio\"");

      *has_audio = false;
      if (q != NULL)
        {
          q += strlen("\"has_audio\"");
          while (*q == ' ' || *q == ':' || *q == '\t')
            {
              q++;
            }

          if (strncmp(q, "true", 4) == 0)
            {
              *has_audio = true;
            }
        }
    }

  /* ⚠️ 2026-09-10 诊断：设备"不播放语音回复"时，这两行直接区分
   * "服务器没合成音频" / "服务器给了但设备没解析到"。 */

  printf("[Bridge] finalize: resp=%d B text=%d B has_audio=%d\n",
         ret, (int)strlen(text_out),
         (has_audio != NULL && *has_audio) ? 1 : 0);

  if (has_audio == NULL || !*has_audio)
    {
      printf("[Bridge] finalize 原文: %.200s\n", resp);
    }

  return 0;
}

int server_bridge_fetch_audio(const char *filepath)
{
  char path[160];

  if (g_session <= 0 || g_sid[0] == '\0' || g_sb_host[0] == '\0')
    {
      return -ENOTCONN;
    }

  snprintf(path, sizeof(path),
           SB_API_PREFIX "/voice/audio?session_id=%s", g_sid);
  return sb_http_get_to_file(g_sb_host, g_sb_port, path, filepath,
                             SB_FINALIZE_TMO);
}

/* 当前语音会话的 TTS 音频路径（流式播放用）。 */

int server_bridge_voice_audio_path(char *buf, size_t cap)
{
  if (g_session <= 0 || g_sid[0] == '\0' || g_sb_host[0] == '\0' ||
      buf == NULL || cap == 0)
    {
      return -ENOTCONN;
    }

  snprintf(buf, cap, SB_API_PREFIX "/voice/audio?session_id=%s", g_sid);
  return 0;
}

/* 按服务器路径直接下载到文件（plant voice get <path> <file>）。
 * 例：server_bridge_fetch_path("/api/v1/media/md_xxx/file",
 *                              "/mnt/sd/tts_test.wav")
 * 用于把"下载→落盘→播放"这一段单独验证。返回写入字节数。 */

int server_bridge_fetch_path(const char *path, const char *filepath)
{
  if (g_sb_host[0] == '\0' || path == NULL || filepath == NULL)
    {
      return -ENOTCONN;
    }

  return sb_http_get_to_file(g_sb_host, g_sb_port, path, filepath,
                             SB_FINALIZE_TMO);
}

/****************************************************************************
 * 协议 v2 图像诊断：两步（/api/v1/media/image → /api/v1/ai/diagnose）。
 * 服务器把 RGB565 转 JPEG 后走 MiMo 视觉，返回结构化 result。
 ****************************************************************************/

static int server_bridge_image_diagnose_ex(const uint8_t *frame,
                                           size_t frame_len,
                                           struct sb_diag_result_s *out,
                                           bool fast_fail)
{
  char path[128];
  char media_id[64];
  char body[128];
  static char resp_media[SB_RESP_MEDIA];   /* media 上传响应 */
  static char resp[SB_RESP_BIG];            /* diagnose 响应（大） */
  const char *rp;                           /* result 对象值起点 */
  const char *re;                           /* result 对象结束 */
  const char *vp;
  int ret;

  if (out == NULL)
    {
      return -EINVAL;
    }

  memset(out, 0, sizeof(*out));
  out->match = -1;
  out->health_score = -1;

  if (g_sb_host[0] == '\0')
    {
      return -ENOTCONN;
    }

  if (g_sb_token[0] == '\0')
    {
      printf("[Bridge] 设备未注册（先 plant voice server <ip>）\n");
      return -EPERM;
    }

  if (frame == NULL || frame_len == 0)
    {
      printf("[Bridge] media/image 参数错误: frame=%p len=%u\n",
             frame, (unsigned)frame_len);
      return -EINVAL;
    }

  /* ① 上传原始帧拿 media_id；按 SOI 标记自动区分 JPEG / raw565。 */

  if (frame_len >= 2 && frame[0] == 0xff && frame[1] == 0xd8)
    {
      snprintf(path, sizeof(path), SB_API_PREFIX "/media/image?fmt=jpeg");
    }
  else
    {
      snprintf(path, sizeof(path), SB_API_PREFIX "/media/image?fmt=raw565");
    }
  /* 2026-09-09 根因澄清：此前误判为“WiFi 刚关联后大包上传 EINVAL”，
   * 实为调用方在驱动 DMA 初始化前取帧得到 NULL（见 cmd_srv diag），
   * send(NULL, 1024) 触发内核 EINVAL。此处保留有界退避仅作网络瞬时
   * 故障保险；frame==NULL 已在下方提前拦截，不进入重试。 */

  {
    static const int waits_ms[7] =
      { 2000, 5000, 10000, 20000, 30000, 45000, 60000 };
    static const int fast_waits_ms[2] = { 1000, 2000 };
    int attempts = fast_fail ? 3 : 8;
    int timeout_ms = fast_fail ? 6000 : SB_MEDIA_TMO;
    int attempt;

    /* 2026-09-27 (v55): fast_fail 从 1 次改为 3 次短退避重试。
     * 实测 WiFi 链路会在 50KB 上传 body 中段瞬时断流（errno=11），
     * 但同期心跳秒级恢复 —— 单次放弃直接把整次 AI 识别判死太冤；
     * 断网时连接失败是秒回的，3 次尝试代价只有几秒。 */

    for (attempt = 0; attempt < attempts; attempt++)
      {
        ret = sb_http_post(g_sb_host, g_sb_port, path,
                           "application/octet-stream",
                           frame, frame_len,
                           resp_media, sizeof(resp_media), timeout_ms);
        if (ret >= 0)
          {
            break;
          }

        if (attempt + 1 < attempts)
          {
            int wait_ms = fast_fail ? fast_waits_ms[attempt] :
                                      waits_ms[attempt];

            printf("[Bridge] media/image 上传失败(%d) 第%d/%d次，%d秒后重试\n",
                   ret, attempt + 1, attempts, wait_ms / 1000);
            usleep((useconds_t)wait_ms * 1000);
          }
        else
          {
            printf("[Bridge] media/image 上传失败(%d) 第%d/%d次，放弃\n",
                   ret, attempt + 1, attempts);
          }
      }
  }
  if (ret < 0)
    {
      printf("[Bridge] media/image 上传失败: %d\n", ret);
      return ret;
    }

  {
    const char *mp = sb_find_key(resp_media, "media");

    media_id[0] = '\0';
    if (mp != NULL && *mp == '{')
      {
        const char *me = sb_matching_end(mp);
        const char *idp = sb_find_key_n(mp + 1, me, "id");

        if (idp != NULL)
          {
            sb_json_str_at(idp, media_id, sizeof(media_id));
          }
      }
  }

  if (media_id[0] == '\0')
    {
      printf("[Bridge] media/image 响应无 media.id (%.100s)\n", resp_media);
      return -EINVAL;
    }

  printf("[Bridge] 图片已上传 media_id=%s\n", media_id);

  /* ② 发起 AI 诊断 */

  snprintf(body, sizeof(body), "{\"media_id\":\"%s\"}", media_id);
  ret = sb_http_post(g_sb_host, g_sb_port,
                     SB_API_PREFIX "/ai/diagnose",
                     "application/json",
                     (const uint8_t *)body, strlen(body),
                     resp, sizeof(resp), SB_DIAG_TMO);
  if (ret < 0)
    {
      printf("[Bridge] ai/diagnose 请求失败: %d\n", ret);
      return ret;
    }

  /* result{} 作用域内解析（防止 raw_model_text 字符串里的同名 key） */

  rp = sb_find_key(resp, "result");
  if (rp == NULL || *rp != '{')
    {
      printf("[Bridge] ai/diagnose 响应无 result (%.100s)\n", resp);
      return -EINVAL;
    }

  re = sb_matching_end(rp);
  if (re == NULL)
    {
      return -EINVAL;
    }

  vp = sb_find_key_n(rp + 1, re, "name");
  if (vp != NULL)
    {
      sb_json_str_at(vp, out->name, sizeof(out->name));
    }

  vp = sb_find_key_n(rp + 1, re, "latin");
  if (vp != NULL)
    {
      sb_json_str_at(vp, out->latin, sizeof(out->latin));
    }

  vp = sb_find_key_n(rp + 1, re, "match");
  if (vp != NULL)
    {
      sb_json_int_at(vp, &out->match);
    }

  vp = sb_find_key_n(rp + 1, re, "health_score");
  if (vp != NULL)
    {
      sb_json_int_at(vp, &out->health_score);
    }

  vp = sb_find_key_n(rp + 1, re, "summary");
  if (vp != NULL)
    {
      sb_json_str_at(vp, out->summary, sizeof(out->summary));
    }

  vp = sb_find_key_n(rp + 1, re, "problems");
  if (vp != NULL && *vp == '[')
    {
      sb_collect_issues(vp, out->issue, sizeof(out->issue), 2);
    }

  vp = sb_find_key_n(rp + 1, re, "suggestions");
  if (vp != NULL && *vp == '[')
    {
      sb_collect_lines(vp, "text", out->advice, sizeof(out->advice), 3);
    }

  out->ok = true;
  printf("[Bridge] 诊断: %s 健康分=%d 匹配=%d\n",
         out->name[0] != '\0' ? out->name : "(未知)", out->health_score,
         out->match);
  return 0;
}

int server_bridge_image_diagnose(const uint8_t *frame, size_t frame_len,
                                 struct sb_diag_result_s *out)
{
  return server_bridge_image_diagnose_ex(frame, frame_len, out, false);
}

int server_bridge_image_diagnose_fast(const uint8_t *frame, size_t frame_len,
                                      struct sb_diag_result_s *out)
{
  return server_bridge_image_diagnose_ex(frame, frame_len, out, true);
}

/* 兼容壳：结构化诊断拼文本（名字/健康分/问题/建议/总结，UI 可显示） */

int server_bridge_image_analyze(const uint8_t *frame, size_t frame_len,
                                char *text_out, size_t text_size)
{
  struct sb_diag_result_s diag;
  int ret;

  if (text_out == NULL || text_size == 0)
    {
      return -EINVAL;
    }

  ret = server_bridge_image_diagnose(frame, frame_len, &diag);
  if (ret < 0 || !diag.ok)
    {
      text_out[0] = '\0';
      return (ret < 0) ? ret : -EINVAL;
    }

  snprintf(text_out, text_size, "%s\n健康 %d 分\n%s%s%s",
           diag.name[0] != '\0' ? diag.name : "识别结果",
           diag.health_score,
           diag.issue[0] != '\0' ? diag.issue : "",
           diag.advice[0] != '\0' ? diag.advice : "",
           diag.summary);
  return 0;
}


int server_bridge_configured(void)
{
  return (g_sb_host[0] != '\0' && g_sb_token[0] != '\0');
}

/* 传感器单条上报：/api/v1/telemetry/put（JSON 体；服务器按 (device,ts) 幂等）
 *
 * 2026-09-11：从 4 参数扩到土壤传感器的全部 8 个真实参数（温度/水分/EC/pH/
 * 盐分/氮/磷/钾）。原来只传 moisture/temp/light/ec——light 根本没有硬件，
 * 是估出来的假值，盐分和氮磷钾采到了却没往上送。现在按真实值上报，
 * 服务器 telemetry 表已加对应列（见 plant_server app/db.py 迁移）。 */

int server_bridge_report_telemetry(float moisture, float temp, float ec,
                                   float ph, float salt, float nitrogen,
                                   float phosphorus, float potassium)
{
  char body[224];
  static char resp[1024];
  int ret;

  if (g_sb_host[0] == '\0' || g_sb_token[0] == '\0')
    {
      return -ENOTCONN;
    }

  snprintf(body, sizeof(body),
           "{\"moisture\":%.1f,\"temp\":%.1f,\"ec\":%.1f,\"ph\":%.1f,"
           "\"salt\":%.1f,\"nitrogen\":%.1f,\"phosphorus\":%.1f,"
           "\"potassium\":%.1f}",
           moisture, temp, ec, ph, salt, nitrogen, phosphorus, potassium);

  ret = sb_http_post(g_sb_host, g_sb_port,
                     SB_API_PREFIX "/telemetry/put",
                     "application/json",
                     (const uint8_t *)body, strlen(body),
                     resp, sizeof(resp), SB_TIMEOUT_MS * 2);
  if (ret < 0)
    {
      printf("[Bridge] telemetry/put 失败: %d\n", ret);
      return ret;
    }

  printf("[Bridge] telemetry ok (%d B): %s\n", ret, resp);
  return 0;
}

/****************************************************************************
 * 心跳保活：POST /api/v1/devices/heartbeat（空 JSON 体，带 X-Device-Token）。
 *
 * 服务器收到即刷新该设备 last_seen_at → 管理台"在线"判定不再只看开机注册
 * （2026-09-11 板卡"假离线"根因：固件从不发心跳，last_seen_at 停在开机那一次）。
 *
 * 响应形如 {"ok":true,"config":{"upload_interval_s":600,"heartbeat_s":60}}，
 * 其中 config.heartbeat_s 回填 interval_s_out，服务器可远程调节心跳周期
 * （固件默认 60s，见 ui_app.c UI_HB_INTERVAL_SEC）。
 *
 * 未配置服务器或尚未注册（无 token）返回 -ENOTCONN，不发请求——无 token 时
 * 服务器会 401，白跑一趟还刷不上 last_seen_at。
 ****************************************************************************/

/* ── 无线土壤：从服务器拉当前传感器 ─────────────────────────────────
 * GET /api/v1/telemetry/soil/now（自动带 X-Device-Token）。
 * The server resolves the soil sensor bound to this plant's owner and
 * returns its latest_telemetry.  This keeps the raw sensor device token
 * out of the plant firmware.  Local hardware failure falls back here. */

static int sb_json_float_at(const char *p, float *out)
{
  char *endp;
  double v;

  if (p == NULL)
    {
      return -EINVAL;
    }

  v = strtod(p, &endp);
  if (endp == p)
    {
      return -EINVAL;
    }

  *out = (float)v;
  return 0;
}

static int sb_soil_field(const char *obj, const char *key, float *out)
{
  const char *p;

  if (obj == NULL)
    {
      return -EINVAL;
    }

  p = sb_find_key(obj, key);
  if (p == NULL)
    {
      return -ENOENT;
    }

  return sb_json_float_at(p, out);
}

int server_bridge_fetch_soil(struct sb_soil_s *out)
{
  static char resp[1024];   /* v191: 收敛，sync 响应够用 */
  const char *lt;
  const char *end;
  int ret;

  if (out == NULL)
    {
      return -EINVAL;
    }

  memset(out, 0, sizeof(*out));

  if (g_sb_host[0] == '\0' || g_sb_token[0] == '\0')
    {
      return -ENOTCONN;
    }

  ret = sb_http_get(g_sb_host, g_sb_port,
                    SB_API_PREFIX "/telemetry/soil/now",
                    resp, sizeof(resp), SB_TIMEOUT_MS * 2);
  if (ret < 0)
    {
      printf("[Bridge] fetch_soil http %d\n", ret);
      return ret;
    }

  lt = sb_find_key(resp, "latest_telemetry");
  if (lt == NULL || *lt != '{')
    {
      printf("[Bridge] fetch_soil parse fail resp=%.80s\n", resp);
      return -ENOENT;
    }

  end = sb_matching_end(lt);
  if (end == NULL)
    {
      return -EINVAL;
    }

  /* 在 latest_telemetry 对象范围内取字段，避免匹配到外层同名 */

  sb_soil_field(lt, "moisture", &out->moisture);
  sb_soil_field(lt, "temp", &out->temp);
  sb_soil_field(lt, "ec", &out->ec);
  sb_soil_field(lt, "ph", &out->ph);
  sb_soil_field(lt, "salt", &out->salt);
  sb_soil_field(lt, "nitrogen", &out->nitrogen);
  sb_soil_field(lt, "phosphorus", &out->phosphorus);
  sb_soil_field(lt, "potassium", &out->potassium);

  /* 全 0 视为没数（服务器该字段为 null 时走不到这里） */

  if (out->moisture == 0.0f && out->temp == 0.0f && out->ec == 0.0f)
    {
      return -ENOENT;
    }

  out->valid = 1;
  printf("[Bridge] cloud soil 水分=%.1f 温度=%.1f EC=%.0f pH=%.1f\n",
         out->moisture, out->temp, out->ec, out->ph);
  return 0;
}


int server_bridge_heartbeat(int *interval_s_out)
{
  static char resp[512];
  int ret;

  if (g_sb_host[0] == '\0' || g_sb_token[0] == '\0')
    {
      return -ENOTCONN;
    }

  ret = sb_http_post(g_sb_host, g_sb_port,
                     SB_API_PREFIX "/devices/heartbeat",
                     "application/json",
                     (const uint8_t *)"{}", 2,
                     resp, sizeof(resp), SB_TIMEOUT_MS * 2);
  if (ret < 0)
    {
      printf("[Bridge] heartbeat 失败: %d\n", ret);
      return ret;
    }

  /* 服务器下发的周期（可选）：解析失败保持固件默认周期 */

  if (interval_s_out != NULL)
    {
      const char *q = strstr(resp, "\"heartbeat_s\"");

      if (q != NULL)
        {
          long v;

          q += strlen("\"heartbeat_s\"");
          while (*q == ' ' || *q == ':' || *q == '\t')
            {
              q++;
            }

          v = strtol(q, NULL, 10);
          if (v > 0 && v <= 3600)
            {
              *interval_s_out = (int)v;
            }
        }
    }

  /* 服务器下发的传感器上报周期：以服务器为准，拿不到就保持本地默认 */

  {
    const char *q = strstr(resp, "\"upload_interval_s\"");

    if (q != NULL)
      {
        long v;

        q += strlen("\"upload_interval_s\"");
        while (*q == ' ' || *q == ':' || *q == '\t')
          {
            q++;
          }

        v = strtol(q, NULL, 10);
        if (v >= 10 && v <= 86400)
          {
            g_upload_interval_s = (int)v;
          }
      }
  }

  printf("[Bridge] heartbeat ok: %s\n", resp);
  return 0;
}

/****************************************************************************
 * 实时时间：GET /api/v1/time → 服务器返回北京时间（UTC+8）。
 * unix_beijing 填北京时间 unix 秒（已偏移），date_out 填 "YYYY-MM-DD"，
 * time_out 填 "HH:MM"。响应 <1KB（静态缓冲）；失败返回负 errno
 * （-ENOTCONN=未配置服务器）。
 ****************************************************************************/

int server_bridge_get_time(int64_t *unix_beijing,
                           char *date_out, size_t date_size,
                           char *time_out, size_t time_size)
{
  static char resp[1024];   /* static: 8KB worker 栈放不下 1KB */
  int ret;

  if (g_sb_host[0] == '\0')
    {
      return -ENOTCONN;
    }

  ret = sb_http_get(g_sb_host, g_sb_port, SB_API_PREFIX "/time",
                    resp, sizeof(resp), SB_TIMEOUT_MS);
  if (ret < 0)
    {
      return ret;
    }

  if (unix_beijing != NULL)
    {
      *unix_beijing = 0;

      /* "unix_beijing" 是 JSON 整数（无引号），单独解析 */

      {
        const char *q = strstr(resp, "\"unix_beijing\"");

        if (q != NULL)
          {
            q += strlen("\"unix_beijing\"");
            while (*q == ' ' || *q == ':' || *q == '\t')
              {
                q++;
              }

            *unix_beijing = (int64_t)strtoll(q, NULL, 10);
          }
      }
    }

  if (date_out != NULL && date_size > 0)
    {
      if (sb_json_get_string(resp, "date", date_out, date_size) < 0)
        {
          date_out[0] = '\0';
        }
    }

  if (time_out != NULL && time_size > 0)
    {
      if (sb_json_get_string(resp, "time", time_out, time_size) < 0)
        {
          time_out[0] = '\0';
        }
    }

  return 0;
}

/****************************************************************************
 * 执行器（自动执行层，2026-09-12）
 *
 * 板卡侧三步：声明能力 → 取指令 → 回执。驱动实现在 actuator_service.c，
 * 本文件只管协议（拼 JSON、发请求、解析响应），跟其它 server_bridge 函数
 * 共用同一套裸 socket 帮手。
 *
 * 为什么 detail 只收 ASCII：JSON 字符串里塞中文要处理转义，设备侧能省则省；
 * 服务器把 detail 原样写进日记，英文短语足够看懂"真浇了还是干跑"。
 ****************************************************************************/

/* 把 src 拷成 JSON 安全串：可打印 ASCII 里排除 " 和 \，其余（含中文/控制符）
 * 一律换成 '_'。设备侧发出的 job_id / detail 都是自己可控的，这样最省心。 */

static void sb_json_safe(char *dst, size_t cap, const char *src)
{
  size_t n = 0;

  if (dst == NULL || cap == 0)
    {
      return;
    }

  if (src != NULL)
    {
      while (*src != '\0' && n + 1 < cap)
        {
          unsigned char c = (unsigned char)*src++;

          if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
            {
              dst[n++] = (char)c;
            }
          else
            {
              dst[n++] = '_';
            }
        }
    }

  dst[n] = '\0';
}

int server_bridge_declare_capabilities(const struct sb_act_cap_s *caps, int n)
{
  char body[384];
  static char resp[1024];
  size_t used;
  int ret;
  int i;

  if (g_sb_host[0] == '\0' || g_sb_token[0] == '\0')
    {
      return -ENOTCONN;
    }

  if (caps == NULL || n <= 0)
    {
      return -EINVAL;
    }

  used = (size_t)snprintf(body, sizeof(body), "{\"actuators\":[");

  for (i = 0; i < n; i++)
    {
      if (used + 64 >= sizeof(body))
        {
          break;
        }

      used += (size_t)snprintf(body + used, sizeof(body) - used,
                               "%s{\"kind\":\"%s\",\"name\":\"%s\"}",
                               i > 0 ? "," : "", caps[i].kind, caps[i].name);
    }

  if (used + 4 < sizeof(body))
    {
      snprintf(body + used, sizeof(body) - used, "]}");
    }
  else
    {
      body[sizeof(body) - 3] = ']';
      body[sizeof(body) - 2] = '}';
      body[sizeof(body) - 1] = '\0';
    }

  ret = sb_http_post(g_sb_host, g_sb_port,
                     SB_API_PREFIX "/actuators/capabilities",
                     "application/json",
                     (const uint8_t *)body, strlen(body),
                     resp, sizeof(resp), SB_TIMEOUT_MS * 2);
  if (ret < 0)
    {
      return ret;
    }

  printf("[Bridge] 执行器声明 ok: %s\n", resp);
  return 0;
}

int server_bridge_fetch_pending(struct sb_act_job_s *out, int max, int *n_out)
{
  static char resp[1536];
  const char *lim;
  const char *p;
  const char *end;
  int n = 0;
  int ret;

  if (n_out != NULL)
    {
      *n_out = 0;
    }

  if (g_sb_host[0] == '\0' || g_sb_token[0] == '\0')
    {
      return -ENOTCONN;
    }

  if (out == NULL || max <= 0)
    {
      return -EINVAL;
    }

  ret = sb_http_get(g_sb_host, g_sb_port, SB_API_PREFIX "/actuators/pending",
                    resp, sizeof(resp), SB_TIMEOUT_MS * 2);
  if (ret < 0)
    {
      printf("[Bridge] 取执行指令失败: %d\n", ret);
      return ret;
    }

  p = sb_find_key(resp, "jobs");
  if (p == NULL || *p != '[')
    {
      return 0;                 /* 没有 jobs 字段：当作"暂时没活" */
    }

  end = sb_matching_end(p);
  lim = (end != NULL) ? end : (resp + strlen(resp));

  while (n < max && p < lim)
    {
      const char *obj_end;
      const char *olim;
      const char *v;

      while (p < lim && *p != '{')
        {
          p++;
        }

      if (p >= lim)
        {
          break;
        }

      obj_end = sb_matching_end(p);
      olim = (obj_end != NULL) ? obj_end : lim;

      memset(&out[n], 0, sizeof(out[n]));
      out[n].duration_s = 0;

      v = sb_find_key_n(p, olim, "job_id");
      if (v == NULL || sb_json_str_at(v, out[n].job_id,
                                      sizeof(out[n].job_id)) <= 0)
        {
          p = olim;               /* 没 id 的条目跳过 */
          continue;
        }

      v = sb_find_key_n(p, olim, "actuator_id");
      if (v != NULL)
        {
          sb_json_str_at(v, out[n].actuator_id, sizeof(out[n].actuator_id));
        }

      v = sb_find_key_n(p, olim, "kind");
      if (v != NULL)
        {
          sb_json_str_at(v, out[n].kind, sizeof(out[n].kind));
        }

      v = sb_find_key_n(p, olim, "action");
      if (v != NULL)
        {
          sb_json_str_at(v, out[n].action, sizeof(out[n].action));
        }

      v = sb_find_key_n(p, olim, "duration_s");
      if (v != NULL)
        {
          sb_json_int_at(v, &out[n].duration_s);
        }

      v = sb_find_key_n(p, olim, "reason");
      if (v != NULL)
        {
          sb_json_str_at(v, out[n].reason, sizeof(out[n].reason));
        }

      printf("[Bridge] 取到执行指令 job=%s kind=%s %ds\n",
             out[n].job_id, out[n].kind, out[n].duration_s);

      n++;
      p = olim;
    }

  if (n_out != NULL)
    {
      *n_out = n;
    }

  return 0;
}

int server_bridge_ack_job(const char *job_id, int ok, const char *detail)
{
  char safe_id[48];
  char safe_detail[96];
  char path[128];
  char body[192];
  static char resp[512];
  int ret;

  if (g_sb_host[0] == '\0' || g_sb_token[0] == '\0')
    {
      return -ENOTCONN;
    }

  if (job_id == NULL || job_id[0] == '\0')
    {
      return -EINVAL;
    }

  sb_json_safe(safe_id, sizeof(safe_id), job_id);
  sb_json_safe(safe_detail, sizeof(safe_detail), detail);

  snprintf(path, sizeof(path), SB_API_PREFIX "/actuators/jobs/%s/ack", safe_id);
  snprintf(body, sizeof(body), "{\"ok\":%s,\"detail\":\"%s\"}",
           ok ? "true" : "false", safe_detail);

  ret = sb_http_post(g_sb_host, g_sb_port, path, "application/json",
                     (const uint8_t *)body, strlen(body),
                     resp, sizeof(resp), SB_TIMEOUT_MS * 2);
  if (ret < 0)
    {
      printf("[Bridge] 执行回执失败 job=%s: %d\n", safe_id, ret);
      return ret;
    }

  printf("[Bridge] 执行回执 ok job=%s: %s\n", safe_id, resp);

  /* 回执成功后服务器会给最新状态（actuator.state / job.status），
   * 顺带把服务器下发的自动规则周期带回：目前没有需要缓存的值，
   * 保持响应体打印即可。 */

  return 0;
}


static void sb_url_component(const char *src,char *out,size_t n)
{
  static const char hex[]="0123456789ABCDEF";
  size_t used=0;
  for(const unsigned char *p=(const unsigned char *)src;*p && used+3<n;p++)
    {if((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-')out[used++]=*p;
     else {out[used++]='%';out[used++]=hex[*p>>4];out[used++]=hex[*p&15];}}
  out[used]=0;
}
static char *sb_quote_text(const char *text)
{
  size_t n=strlen(text);char *out=malloc(n*2+3);if(!out)return NULL;
  char *p=out;*p++='"';
  for(size_t i=0;i<n;i++)
    {unsigned char c=text[i];if(c=='"'||c=='\\'){*p++='\\';*p++=c;}
     else if(c=='\n'){*p++='\\';*p++='n';}
     else if(c=='\r'){*p++='\\';*p++='r';}
     else if(c>=32)*p++=c;}
  *p++='"';*p=0;return out;
}
static int sb_vision_decode(const char *response,struct plant_vision_result *v)
{
  memset(v,0,sizeof(*v));
  const char *ok=sb_find_key(response,"ok");
  if(!ok || strncmp(ok,"true",4))return -EIO;
  const char *keys[]={"frame_id","cx","cy","confidence","yaw","pitch","horizontal_sign","vertical_sign"};
  int *values[]={&v->frame_id,&v->cx,&v->cy,&v->confidence,&v->yaw,&v->pitch,&v->horizontal_sign,&v->vertical_sign};
  for(int i=0;i<8;i++){const char *p=sb_find_key(response,keys[i]);if(p)*values[i]=(int)strtol(p,NULL,10);}
  const char *found=sb_find_key(response,"found");v->found=found&&!strncmp(found,"true",4);
  if(v->found && (v->cx<0||v->cx>1000||v->cy<0||v->cy>1000||v->confidence<0||v->confidence>100))return -EINVAL;
  sb_json_get_string(response,"target_id",v->target_id,sizeof(v->target_id));
  sb_json_get_string(response,"label",v->label,sizeof(v->label));
  sb_json_get_string(response,"message",v->message,sizeof(v->message));
  return 0;
}
static int sb_vision_request(const char *mode,const uint8_t *image,size_t bytes,
                        int width,int height,int yaw,int pitch,unsigned frame_id,
                        const char *run,const char *name,struct plant_vision_result *out,bool prefetch_notice)
{
  if(!mode||!image||!run||!name||!out||bytes!=(size_t)width*height*2)return -EINVAL;
  const uint8_t *wire=image;size_t wire_bytes=bytes;uint8_t *jpeg=NULL;
  const char *format="raw565";
#ifdef CONFIG_PLANT_CAMERA_GC0308
  if(strcmp(mode,"faces"))
    {
      uint32_t started=clock_systime_ticks();size_t encoded=0;
      int packed=camera_jpeg_encode_rgb565(image,bytes,width,height,&jpeg,&encoded);
      if(packed==0){wire=jpeg;wire_bytes=encoded;format="jpeg";}
      printf("[Perf][Vision] jpeg=%u ms raw=%u wire=%u rc=%d\n",
             (unsigned)((clock_systime_ticks()-started)*1000/TICK_PER_SEC),
             (unsigned)bytes,(unsigned)wire_bytes,packed);
    }
#endif
  char path[448],encoded[192];sb_url_component(name,encoded,sizeof(encoded));
  snprintf(path,sizeof(path),SB_API_PREFIX "/vision/frame?mode=%s&fmt=%s&width=%d&height=%d&yaw=%d&pitch=%d&frame_id=%u&run=%s&name=%s%s",mode,format,width,height,yaw,pitch,frame_id,run,encoded,
           prefetch_notice&&!strcmp(mode,"inspect")?"&prefetch_notice=1":"");
  char *response=malloc(4096);if(!response){free(jpeg);return -ENOMEM;}
#ifdef CONFIG_PLANT_CAMERA_GC0308
  uint32_t http_started=clock_systime_ticks();
#endif
  int rc=sb_http_post(g_sb_host,g_sb_port,path,!strcmp(format,"jpeg")?"image/jpeg":"application/octet-stream",wire,wire_bytes,response,4096,!strcmp(mode,"faces")?3500:90000);
  free(jpeg);
#ifdef CONFIG_PLANT_CAMERA_GC0308
  {
    const char *value=rc>=0?sb_find_key(response,"latency_ms"):NULL;
    long server_ms=value?strtol(value,NULL,10):-1;
    if(server_ms<0 || server_ms>90000)server_ms=-1;
    printf("[Perf][Vision] http=%u ms server=%ld ms wire=%u rc=%d\n",
           (unsigned)((clock_systime_ticks()-http_started)*1000/TICK_PER_SEC),
           server_ms,(unsigned)wire_bytes,rc);
  }
#endif
  if(rc>=0)
    {
      int received=rc;
      rc=sb_vision_decode(response,out);
      if(rc<0)
        {
          char detail[128]="";
          sb_json_get_string(response,"detail",detail,sizeof(detail));
          printf("[Vision] response rejected mode=%s frame=%u bytes=%d rc=%d detail=%.100s\n",mode,frame_id,received,rc,detail);
          /* The image was received, but the upstream semantic answer had
           * multiple JSON values. No usable observation was returned. */
          if(!strncmp(detail,"Extra data:",11))rc=-EBADMSG;
        }
    }
  else printf("[Vision] transport failed mode=%s frame=%u rc=%d\n",mode,frame_id,rc);
  free(response);return rc;
}
int server_bridge_vision(const char *mode,const uint8_t *image,size_t bytes,
                        int width,int height,int yaw,int pitch,unsigned frame_id,
                        const char *run,const char *name,struct plant_vision_result *out)
{return sb_vision_request(mode,image,bytes,width,height,yaw,pitch,frame_id,run,name,out,false);}
int server_bridge_vision_spoken(const char *mode,const uint8_t *image,size_t bytes,
                        int width,int height,int yaw,int pitch,unsigned frame_id,
                        const char *run,const char *name,struct plant_vision_result *out)
{return sb_vision_request(mode,image,bytes,width,height,yaw,pitch,frame_id,run,name,out,true);}

int server_bridge_target(const char *name,struct plant_vision_result *out)
{
  char path[288],encoded[192];sb_url_component(name,encoded,sizeof(encoded));
  snprintf(path,sizeof(path),SB_API_PREFIX "/vision/target?name=%s",encoded);
  char *response=malloc(2048);if(!response)return -ENOMEM;
  int rc=sb_http_get(g_sb_host,g_sb_port,path,response,2048,10000);
  if(rc>=0)rc=sb_vision_decode(response,out);
  free(response);return rc;
}
int server_bridge_remember(const char *target_id,const char *name,struct plant_vision_result *out)
{
  if(!target_id||!name||strlen(target_id)>31||strlen(name)>63)return -EINVAL;
  char *quoted=sb_quote_text(name);if(!quoted)return -ENOMEM;
  char body[256];snprintf(body,sizeof(body),"{\"target_id\":\"%s\",\"name\":%s}",target_id,quoted);free(quoted);
  char *response=malloc(2048);if(!response)return -ENOMEM;
  int rc=sb_http_post(g_sb_host,g_sb_port,SB_API_PREFIX "/vision/name","application/json",(const uint8_t *)body,strlen(body),response,2048,10000);
  if(rc>=0)rc=sb_vision_decode(response,out);
  free(response);return rc;
}
int server_bridge_notice(const char *text)
{
  char *quoted=sb_quote_text(text);if(!quoted)return -ENOMEM;
  size_t n=strlen(quoted)+48;char *body=malloc(n),*response=malloc(2048);
  if(!body||!response){free(quoted);free(body);free(response);return -ENOMEM;}
  snprintf(body,n,"{\"text\":%s,\"stream_audio\":true}",quoted);free(quoted);
  g_session=0;g_sid[0]='\0';g_voice_reply_pending=false;
  int rc=sb_http_post(g_sb_host,g_sb_port,SB_API_PREFIX "/voice/notice","application/json",(const uint8_t *)body,strlen(body),response,2048,90000);
  if(rc>=0)
    {const char *audio=sb_find_key(response,"has_audio");
     if(!audio || strncmp(audio,"true",4)||sb_json_get_string(response,"session_id",g_sid,sizeof(g_sid))<0 || !g_sid[0])rc=-EIO;
     else {g_session=1;g_seq=0;g_voice_action[0]=0;g_voice_arg[0]=0;rc=0;}}
  free(body);free(response);return rc;
}

int server_bridge_reset_scene(void)
{
  char *response=malloc(1024);if(!response)return -ENOMEM;
  int rc=sb_http_post(g_sb_host,g_sb_port,SB_API_PREFIX "/vision/reset","application/json",(const uint8_t *)"{}",2,response,1024,10000);
  const char *ok=rc>=0?sb_find_key(response,"ok"):NULL;
  if(rc>=0)rc=ok&&!strncmp(ok,"true",4)?0:-EIO;
  free(response);return rc;
}

/* Diagnostic only: private LAN collector; no production voice session/action.
 * Collector authenticates the device against the normal upstream service.
 */
int server_bridge_nn_pair_upload(const char *tag,const int16_t *pcm,size_t samples)
{
  if(!tag||!tag[0]||!pcm||!samples||samples>128000u)return -EINVAL;
  size_t n=strlen(tag);if(n>48)return -EINVAL;
  for(size_t i=0;i<n;i++)
    if(!((tag[i]>='a'&&tag[i]<='z')||(tag[i]>='A'&&tag[i]<='Z')||
         (tag[i]>='0'&&tag[i]<='9')||tag[i]=='_'||tag[i]=='-'))return -EINVAL;
  /* Prevent accidentally exporting diagnostic audio to production or an
   * arbitrary URL. Existing300s/900s LAN collector uses this dedicated port. */
  if(g_sb_port!=18089 || strcmp(g_sb_host,"192.0.2.1"))return -EPERM;
  uint32_t crc=~0u;const uint8_t *bytes=(const uint8_t *)pcm;
  for(size_t i=0;i<samples*2;i++)
    {crc^=bytes[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^(0xedb88320u&-(crc&1));}
  crc=~crc;char path[160],response[64];
  snprintf(path,sizeof(path),SB_API_PREFIX "/voice/_nn_pair?tag=%s&crc=%08lx",tag,(unsigned long)crc);
  int rc=sb_http_post(g_sb_host,g_sb_port,path,"application/octet-stream",bytes,samples*2,response,sizeof(response),30000);
  if(rc<0)return rc;
  return rc==7&&!memcmp(response,"NNPAIR1",7)?0:-EIO;
}
