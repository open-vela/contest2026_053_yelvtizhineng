/****************************************************************************
 * apps/plant-companion/communication/wifi_manager/wifi_manager.c
 *
 * WiFi Manager — NuttX WiFi connection management.
 *
 * Uses NuttX wapi tool or ioctl to connect to WiFi networks.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netutils/netlib.h>

#include "wifi_manager.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_INTERFACE "wlan0"

/* wapi 子命令的输出**不落盘**（2026-09-17 第三次返工，别再改回文件）。
 *
 * 病史：
 *   - 最早写 /mnt/sd/wifi_scan.txt：SD 没挂载时扫不出东西；
 *   - 2026-09-15 改到 /data：/data 是挂在片内 SPI Flash 上的 littlefs
 *     （nuttx/boards/xtensa/esp32s3/common/src/esp32s3_board_spiflash.c 的
 *     setup_littlefs(..., "/data", ...)），写它要擦 flash 扇区，实测进异常
 *     分支后整机静默锁死："点 WiFi 图标后界面/网络/串口一起没反应"；
 *   - SD 也不安全：FAT 写入走 SDMMC DMA，驱动断言要求缓冲 4 字节对齐
 *     （见 services/sd_write.h），stdio 分块 flush 传的"基址+偏移"指针可能
 *     不满足 → 断言/整机挂死（实测崩在 FAT 的互斥锁上）。
 *
 * 扫描结果本来就只需要在内存里解析，直接管子进程的 stdout（wifi_run_capture）。 */
/* wapi 操作串行化锁：扫描 / 连接 / 断开 / 自愈重连都改同一份 WiFi 状态，
 * 并发跑会互相打断（用户点连接时正好自愈重连等）。 */
static pthread_mutex_t g_op_lock = PTHREAD_MUTEX_INITIALIZER;

static bool g_initialized = false;
static bool g_connected = false;
static char g_ssid[33] = {0};
static char g_password[65] = {0};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 把 wlan0 的 IPv4 地址清成 0.0.0.0。
 *
 * ⚠ 2026-09-15：esp32 驱动换了 AP 之后不会让旧地址失效。实测连上
 * tadyhotpoint 后 `ifconfig wlan0` 显示的还是上一个网络的 192.168.3.69，
 * 于是 ARP 一直发给旧网关（arp_send: ERROR: arp_wait failed: -110
 * ipaddr: 192.168.3.1），心跳全部 -101，云端一直看不到设备；同时
 * wifi_manager_has_ip() 会被旧地址骗过，让连接"看起来成功"，人就一直
 * 卡在假连接里。先清成 0.0.0.0，后面 renew 才会真的重新走一遍 DHCP。 */


/* 前置声明：下面的 get_essid / scan 都要用它（定义在文件后半段） */
static int wifi_run_capture(char *argv[], char *out, size_t cap);
static void wifi_clear_ip(void)
{
  struct ifreq ifr;
  struct sockaddr_in *sin;
  int sd;

  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sd < 0)
    {
      return;
    }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, WIFI_INTERFACE, IFNAMSIZ - 1);

  sin = (struct sockaddr_in *)&ifr.ifr_addr;
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = INADDR_ANY;

  if (ioctl(sd, SIOCSIFADDR, (unsigned long)(uintptr_t)&ifr) < 0)
    {
      printf("[WIFI] 清旧 IP 失败: %d\n", errno);
    }

  close(sd);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int wifi_manager_init(void)
{
  if (g_initialized)
    {
      return 0;
    }

  /* WiFi is initialized by board bringup */

  g_initialized = true;
  return 0;
}

static int wifi_connect_ex(const char *ssid, const char *password,
                           bool allow_rollback)
{
  char *argv[8];
  pid_t pid;
  int ret;
  int status;
  char prev_ssid[33];
  char prev_pwd[65];
  bool had_prev;

  if (!g_initialized)
    {
      return -ENODEV;
    }

  if (!ssid || !password)
    {
      return -EINVAL;
    }

  /* 记下"这次要顶掉的旧网络"：新网络连不上时用它退回去（见本函数末尾）。
   * 必须在下面 strncpy(g_ssid, ...) 之前抄，否则抄到的就是新值。 */

  strncpy(prev_ssid, g_ssid, sizeof(prev_ssid) - 1);
  prev_ssid[sizeof(prev_ssid) - 1] = '\0';
  strncpy(prev_pwd, g_password, sizeof(prev_pwd) - 1);
  prev_pwd[sizeof(prev_pwd) - 1] = '\0';

  had_prev = (prev_ssid[0] != '\0' &&
              (strcmp(prev_ssid, ssid) != 0 ||
               strcmp(prev_pwd, password) != 0));

  /* ⚠ 2026-09-11：连接前**先断开**。
   *
   * esp32 驱动在"已经连着同一个 AP"时，如果新的密码设不进去（比如少于 8 位
   * 被 wapi 拒绝），旧关联和旧密钥会继续生效 → 看起来"连接成功"，
   * 然后把错误配置写进 SD，下次开机永远连不上。
   * 实测证据：_work/wrongpw.log（密码 'aaa' 被拒 → 仍报成功并写盘）。
   * 断开之后旧关联失效，连接结果才可信。 */

  argv[0] = "wapi";
  argv[1] = "disconnect";
  argv[2] = "wlan0";
  argv[3] = NULL;

  if (posix_spawn(&pid, "wapi", NULL, NULL, argv, NULL) == 0)
    {
      waitpid(pid, &status, 0);
    }

  usleep(500 * 1000);

  /* ⚠ 2026-09-15：换网络前先清掉旧 IP（原因见 wifi_clear_ip 注释）。
   * 不清的话 renew 会直接续用上一个网络的地址，连接"看起来成功"但根本
   * 不通，用户换环境后就卡死在这里。 */

  wifi_clear_ip();

  /* Save SSID and password */

  strncpy(g_ssid, ssid, sizeof(g_ssid) - 1);
  g_ssid[sizeof(g_ssid) - 1] = '\0';
  strncpy(g_password, password, sizeof(g_password) - 1);
  g_password[sizeof(g_password) - 1] = '\0';

  printf("[WIFI] Connecting to: %s\n", ssid);

  /* 1) 密码。wapi psk 第 4 参 = WPA 版本（数字=下标）：0=NONE 1=WPA1
   *    2=WPA2 3=WPA3；家用路由器多数 WPA2/WPA3 混合，传 2 最通用。
   *    空密码视为开放网络，跳过本步。 */

  if (password[0] != '\0')
    {
      argv[0] = "wapi";
      argv[1] = "psk";
      argv[2] = "wlan0";
      argv[3] = (char *)password;
      argv[4] = "2";
      argv[5] = NULL;

      ret = posix_spawn(&pid, "wapi", NULL, NULL, argv, NULL);
      if (ret != 0)
        {
          printf("[WIFI] Failed to set password: %d\n", ret);
          g_connected = false;
          return ret;
        }

      waitpid(pid, &status, 0);

      /* wapi 只在密码长度 8~63 时才接受，失败要当场拦住（否则旧密钥当道） */

      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
          printf("[WIFI] 密码被拒绝：WiFi 密码需 8~63 位\n");
          g_connected = false;
          return -EINVAL;
        }
    }
  else
    {
      printf("[WIFI] 空密码：按开放网络处理\n");
    }

  /* 2) SSID —— esp32 的 wapi 在执行这条时就会发起关联 */

  argv[0] = "wapi";
  argv[1] = "essid";
  argv[2] = "wlan0";
  argv[3] = (char *)ssid;
  argv[4] = "1";
  argv[5] = NULL;

  ret = posix_spawn(&pid, "wapi", NULL, NULL, argv, NULL);
  if (ret != 0)
    {
      printf("[WIFI] Failed to set SSID: %d\n", ret);
      g_connected = false;
      return ret;
    }

  waitpid(pid, &status, 0);

  /* 3) reconnect —— 部分固件这条不支持，失败不算致命（关联已由 essid 触发） */

  argv[0] = "wapi";
  argv[1] = "reconnect";
  argv[2] = "wlan0";
  argv[3] = NULL;

  if (posix_spawn(&pid, "wapi", NULL, NULL, argv, NULL) == 0)
    {
      waitpid(pid, &status, 0);
    }

  /* 4) 等关联 + DHCP。路由器关联慢时前几次 renew 会失败，多给几次机会。 */

  sleep(2);

  {
    int attempt;
    int ip_try;

    for (attempt = 0; attempt < 4; attempt++)
      {
        printf("[WIFI] DHCP renew attempt %d/4...\n", attempt + 1);
        argv[0] = "renew";
        argv[1] = "wlan0";
        argv[2] = NULL;
        ret = posix_spawn(&pid, "renew", NULL, NULL, argv, NULL);
        if (ret == 0)
          {
            waitpid(pid, &status, 0);
          }
        else
          {
            printf("[WIFI] Failed to get IP (attempt %d): %d\n",
                   attempt + 1, ret);
          }

        sleep(3);

        if (wifi_manager_has_ip())
          {
            break;
          }
      }

    for (ip_try = 0; ip_try < 3 && !wifi_manager_has_ip(); ip_try++)
      {
        printf("[WIFI] 仍未拿到 IP，补试 %d/3...\n", ip_try + 1);
        argv[0] = "renew";
        argv[1] = "wlan0";
        argv[2] = NULL;

        if (posix_spawn(&pid, "renew", NULL, NULL, argv, NULL) == 0)
          {
            waitpid(pid, &status, 0);
          }

        sleep(2);
      }
  }

  /* 5) 成败判定：链路真的 RUNNING（=已关联）且拿到 IP 才算成功。
   *    这里以前是无条件 return 0，是"密码错也报成功"的根源。 */

  if (!wifi_manager_link_up() || !wifi_manager_has_ip())
    {
      printf("[WIFI] 连接 %s 失败：关联=%d 有IP=%d\n", ssid,
             (int)wifi_manager_link_up(), (int)wifi_manager_has_ip());
      g_connected = false;

      /* ⚠ 2026-09-17 自动退回上一个网络。
       *
       * 现场最怕的是：用户换个环境、网络名/密码打错一个字，这台机器就把
       * 旧凭据顶掉了，从此一直连不上、只能断电重启（用户现象"怎么这会又
       * 断网了"）。这里失败即回退，至少保住原来那张网能用。
       *
       * allow_rollback 只允许退一层，回退自身失败就直接返回，不会来回弹。 */

      if (allow_rollback && had_prev)
        {
          printf("[WIFI] 新网络连不上，退回上一个网络：%s\n", prev_ssid);
          return wifi_connect_ex(prev_ssid, prev_pwd, false);
        }

      return -1;
    }

  g_connected = true;
  printf("[WIFI] Connected to %s\n", ssid);

  return 0;
}
static int wifi_connect_locked(const char *ssid, const char *password)
{
  return wifi_connect_ex(ssid, password, true);
}

int wifi_manager_connect(const char *ssid, const char *password)
{
  int ret;

  pthread_mutex_lock(&g_op_lock);
  ret = wifi_connect_locked(ssid, password);
  pthread_mutex_unlock(&g_op_lock);

  return ret;
}
/* 扫描后的关联恢复（2026-09-17）。
 *
 * 实测：`wapi scan wlan0` 跑完之后数据面就断了 —— ifconfig 依然显示
 * at RUNNING + inet addr，但心跳/上报全部 -110（用户现象"怎么这会又断网
 * 了"），而且不会自己恢复。扫描是留在这个页面上就要用的功能，所以扫完
 * 立刻把关联接回去，别等心跳超时去兜底（那要两分钟）。
 *
 * 先用最轻的 `wapi essid`（这一条就会触发重新关联）试一次；没接上再走
 * 完整 connect（会重跑 DHCP，慢但稳）。 */

static void wifi_reassoc_saved(void)
{
  char ssid[33];
  char pwd[65];
  char *argv[6];

  if (g_ssid[0] == '\0')
    {
      return;   /* 没连过网，没什么可恢复的 */
    }

  strncpy(ssid, g_ssid, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  strncpy(pwd, g_password, sizeof(pwd) - 1);
  pwd[sizeof(pwd) - 1] = '\0';

  printf("[WIFI] 扫描后把关联接回来: %s\n", ssid);

  argv[0] = "wapi";
  argv[1] = "essid";
  argv[2] = "wlan0";
  argv[3] = ssid;
  argv[4] = "1";
  argv[5] = NULL;

  {
    pid_t pid;
    int status;

    if (posix_spawn(&pid, "wapi", NULL, NULL, argv, NULL) == 0)
      {
        waitpid(pid, &status, 0);
      }
  }

  sleep(2);

  if (wifi_manager_link_up() && wifi_manager_has_ip())
    {
      printf("[WIFI] 关联已恢复\n");
      return;
    }

  printf("[WIFI] 轻量恢复没成功，改走完整重连\n");
  wifi_connect_locked(ssid, pwd);
}

static int wifi_disconnect_locked(void)
{
  if (!g_initialized)
    {
      return -ENODEV;
    }

  /* ⚠ 2026-09-15：这里原来是空实现（只置了个软件标志）。板子上 STA 其实
   * 还关联着旧 AP，跟换网络 / 扫描打架（wapi scan 会被
   * "STA is connecting, scan are not allowed" 拒掉）。现在真的断开，
   * 顺手把旧 IP 一起清掉。 */

  {
    char *argv[4];
    pid_t pid;
    int status;

    argv[0] = "wapi";
    argv[1] = "disconnect";
    argv[2] = "wlan0";
    argv[3] = NULL;

    if (posix_spawn(&pid, "wapi", NULL, NULL, argv, NULL) == 0)
      {
        waitpid(pid, &status, 0);
      }
  }

  wifi_clear_ip();

  g_ssid[0] = '\0';
  g_password[0] = '\0';
  g_connected = false;
  return 0;
}

int wifi_manager_disconnect(void)
{
  int ret;

  pthread_mutex_lock(&g_op_lock);
  ret = wifi_disconnect_locked();
  pthread_mutex_unlock(&g_op_lock);

  return ret;
}
/* 网络自愈：用"最近一次连接用过的 SSID/密码"重新连一次。
 *
 * 场景（用户现场："怎么这会又断网了"）：扫描或路由器换信道之后，某个时刻
 * 数据面被打断，但 ifconfig 依然显示 at RUNNING + inet addr —— netdev 状态
 * 没跟着变，于是心跳/语音/上报全部 -110 超时，界面上却看不出任何异常，用户
 * 只能断电重启。这里给它一个"自己接回来"的动作。
 *
 * 只在"确实连过网"时才做事（没有凭据就什么都不做）；由调用方（心跳连续
 * 失败）决定何时调，避免正常上网时被它打断。 */


int wifi_manager_reconnect_saved(void)
{
  char ssid[33];
  char pwd[65];
  int ret;

  if (!g_initialized || g_ssid[0] == '\0')
    {
      return -ENOENT;
    }

  /* 必须先抄出来：wifi_connect_locked 内部会 strncpy 回 g_ssid，
   * 直接把 g_ssid 传进去就成了自己拷自己（重叠，结果未定义）。 */

  strncpy(ssid, g_ssid, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  strncpy(pwd, g_password, sizeof(pwd) - 1);
  pwd[sizeof(pwd) - 1] = '\0';

  printf("[WIFI] 网络不通，自动重连 %s\n", ssid);

  pthread_mutex_lock(&g_op_lock);
  ret = wifi_connect_locked(ssid, pwd);
  pthread_mutex_unlock(&g_op_lock);

  return ret;
}

int wifi_manager_get_status(wifi_status_t *status)
{
  if (!status)
    {
      return -EINVAL;
    }

  memset(status, 0, sizeof(wifi_status_t));
  strncpy(status->ssid, g_ssid, sizeof(status->ssid) - 1);
  strncpy(status->password, g_password, sizeof(status->password) - 1);
  status->connected = g_connected;

  return 0;
}

bool wifi_manager_is_connected(void)
{
  return g_connected;
}

/****************************************************************************
 * Public Functions — 扫描
 ****************************************************************************/

/* 从 `wapi show` 的文本里取 "ESSID: xxx" */

static int wifi_essid_from_text(const char *text, char *buf, size_t buflen)
{
  const char *p = text;

  if (text == NULL || buf == NULL || buflen == 0)
    {
      return -EINVAL;
    }

  buf[0] = '\0';

  while ((p = strstr(p, "ESSID:")) != NULL)
    {
      const char *v = p + 6;
      const char *e;
      size_t len;

      p = v;

      while (*v == ' ' || *v == '\t')
        {
          v++;
        }

      e = v;
      while (*e != '\0' && *e != '\r' && *e != '\n')
        {
          e++;
        }

      len = (size_t)(e - v);
      if (len > buflen - 1)
        {
          len = buflen - 1;
        }

      memcpy(buf, v, len);
      buf[len] = '\0';
      return 0;
    }

  return -ENOENT;
}
/* 真实实现（调用方要持锁）；公开入口见文件末尾 */

static int wifi_manager_get_essid_inner(char *buf, size_t buflen)
{
  static char cap[1024];
  char *argv[4];
  int ret;

  if (buf == NULL || buflen == 0)
    {
      return -EINVAL;
    }

  buf[0] = '\0';

  /* ! netlib_getessid（ioctl SIOCGIWESSID）在本板 esp32 驱动上不实现，
   * 实测返回错误且 buffer 空。改用 `wapi show` 的 "ESSID: xxx" 行，
   * 与扫描同一套路（CLI 输出从管道收进内存，不落盘）。 */

  argv[0] = "wapi";
  argv[1] = "show";
  argv[2] = "wlan0";
  argv[3] = NULL;

  ret = wifi_run_capture(argv, cap, sizeof(cap));
  if (ret < 0)
    {
      return ret;
    }

  return wifi_essid_from_text(cap, buf, buflen);
}

/* 只读查询：别人正在扫/连就干脆不查（调用方显示 --），不排队等 */
/* 只读查询：别人正在扫/连就干脆不查（调用方显示 --），不排队干等 */

int wifi_manager_get_essid(char *buf, size_t buflen)
{
  int ret;

  if (pthread_mutex_trylock(&g_op_lock) != 0)
    {
      return -EBUSY;
    }

  ret = wifi_manager_get_essid_inner(buf, buflen);
  pthread_mutex_unlock(&g_op_lock);

  return ret;
}

bool wifi_manager_link_up(void)
{
  struct ifreq ifr;
  int sd;
  bool ok = false;

  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sd < 0)
    {
      return false;
    }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, WIFI_INTERFACE, IFNAMSIZ - 1);

  /* IFF_RUNNING 只有真正关联上才会置位：断开后 ifconfig 显示 "at UP"，
   * 关联成功才是 "at RUNNING"（实测证据 _work/manual_conn.log）。 */

  if (ioctl(sd, SIOCGIFFLAGS, (unsigned long)(uintptr_t)&ifr) == 0)
    {
      ok = ((ifr.ifr_flags & IFF_RUNNING) != 0);
    }

  close(sd);
  return ok;
}

bool wifi_manager_has_ip(void)
{
  struct ifreq ifr;
  int sd;
  bool ok = false;

  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sd < 0)
    {
      return false;
    }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, WIFI_INTERFACE, IFNAMSIZ - 1);

  if (ioctl(sd, SIOCGIFADDR, (unsigned long)(uintptr_t)&ifr) == 0)
    {
      struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;

      ok = (sin->sin_addr.s_addr != INADDR_ANY);
    }

  close(sd);
  return ok;
}

/* 跑一条命令（wapi 等），把它的 stdout 收进内存缓冲 —— 不经过任何文件系统。
 * 返回读到的字节数（>=0）；负 errno = 启动失败。
 *
 * 管道缓冲只有 1KB（CONFIG_DEV_PIPE_SIZE=1024），子进程一口气写几百字节就
 * 可能写满而阻塞，所以必须"边读边等"：先把读端读到 EOF，再 waitpid。
 * 超出 out 容量的部分照样读走丢弃，否则子进程会卡在写管道上永远不退出。 */

static int wifi_run_capture(char *argv[], char *out, size_t cap)
{
  posix_spawn_file_actions_t fa;
  char scratch[128];
  pid_t pid;
  int fds[2];
  int status;
  int ret;
  size_t total = 0;

  if (out == NULL || cap == 0)
    {
      return -EINVAL;
    }

  out[0] = '\0';

  if (pipe(fds) != 0)
    {
      return -errno;
    }

  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);   /* 子进程 stdout -> 管道 */
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);

  ret = posix_spawn(&pid, argv[0], &fa, NULL, argv, NULL);
  posix_spawn_file_actions_destroy(&fa);

  /* 父进程必须关掉写端，否则读端永远等不到 EOF */

  close(fds[1]);

  if (ret != 0)
    {
      close(fds[0]);
      return -ret;
    }

  for (;;)
    {
      ssize_t got;

      if (total < cap - 1)
        {
          got = read(fds[0], out + total, cap - 1 - total);
          if (got > 0)
            {
              total += (size_t)got;
            }
        }
      else
        {
          got = read(fds[0], scratch, sizeof(scratch));   /* 溢出部分丢弃 */
        }

      if (got <= 0)
        {
          break;
        }
    }

  close(fds[0]);
  waitpid(pid, &status, 0);

  out[total] = '\0';
  return (int)total;
}

/* 解析 wapi scan 输出：
 *   bssid / frequency / signal level / encode / ssid
 *   08:9b:4b:18:75:e7	2462	-86	0800	GLHQ2.4G
 * SSID 取第 5 列到行尾（可以有空格）。 */

static int wifi_parse_scan(const char *text, wifi_ap_t *aps, int max)
{
  const char *src = text;
  char line[192];
  int n = 0;
  int i;

  if (text == NULL)
    {
      return -EINVAL;
    }

  while (src != NULL && *src != '\0' && n < max)
    {
      const char *nl = strchr(src, '\n');
      size_t len = (nl != NULL) ? (size_t)(nl - src) : strlen(src);

      if (len >= sizeof(line))
        {
          len = sizeof(line) - 1;
        }

      memcpy(line, src, len);
      line[len] = '\0';
      src = (nl != NULL) ? (nl + 1) : NULL;
      char *f[4];
      char *p = line;
      char *ssid;
      int k;
      int rssi;

      for (k = 0; k < 4; k++)
        {
          char *tab = strchr(p, '\t');

          if (tab == NULL)
            {
              break;
            }

          *tab = '\0';
          f[k] = p;
          p = tab + 1;
        }

      if (k < 4)
        {
          continue;   /* 表头/短行 */
        }

      if (strlen(f[0]) != 17)
        {
          continue;   /* 不是 bssid 行 */
        }

      ssid = p;

      {
        size_t l = strlen(ssid);

        while (l > 0 && (ssid[l - 1] == '\r' || ssid[l - 1] == '\n'))
          {
            ssid[--l] = '\0';
          }
      }

      if (ssid[0] == '\0')
        {
          continue;   /* 隐藏 SSID */
        }

      if (strlen(ssid) > 32)
        {
          ssid[32] = '\0';
        }

      rssi = atoi(f[2]);
      if (rssi == 0)
        {
          continue;
        }

      /* 同一 SSID（多频段/多 BSSID）只留信号最强的一条 */

      for (i = 0; i < n; i++)
        {
          if (strcmp(aps[i].ssid, ssid) == 0)
            {
              break;
            }
        }

      if (i < n)
        {
          if (rssi > aps[i].rssi)
            {
              aps[i].rssi = rssi;
            }

          continue;
        }

      strncpy(aps[n].ssid, ssid, sizeof(aps[n].ssid) - 1);
      aps[n].ssid[sizeof(aps[n].ssid) - 1] = '\0';
      aps[n].rssi = rssi;
      aps[n].secure = (strtoul(f[3], NULL, 16) != 0);
      n++;
    }


  /* 插入排序：信号强的排前面 */

  for (i = 1; i < n; i++)
    {
      wifi_ap_t tmp = aps[i];
      int j = i - 1;

      while (j >= 0 && aps[j].rssi < tmp.rssi)
        {
          aps[j + 1] = aps[j];
          j--;
        }

      aps[j + 1] = tmp;
    }

  printf("[WIFI] 扫描到 %d 个网络\n", n);
  return n;
}

static int wifi_scan_locked(wifi_ap_t *aps, int max)
{
  static char cap[4096];
  char *argv[4];
  int ret;

  if (aps == NULL || max <= 0)
    {
      return -EINVAL;
    }

  if (!g_initialized)
    {
      return -ENODEV;
    }

  /* wapi 没有可链接的 C API → 跑 CLI，stdout 从管道收进内存再解析 */

  argv[0] = "wapi";
  argv[1] = "scan";
  argv[2] = "wlan0";
  argv[3] = NULL;

  ret = wifi_run_capture(argv, cap, sizeof(cap));

  if (ret < 0)
    {
      printf("[WIFI] 扫描启动失败: %d\n", ret);
      return ret;
    }

  ret = wifi_parse_scan(cap, aps, max);

  /* ! 换环境时的典型故障：设备里存的还是旧网络，开机自动连接失败后
   * esp32 停在 "STA is connecting"，此时 wapi scan 被拒
   * （"STA is connecting, scan are not allowed"）→ 一个网络都扫不到，
   * 用户就没法改用新网络。这里先断开让 STA 回到空闲态，再扫一次。 */

  if (ret <= 0)
    {
      printf("[WIFI] 扫描到 %d 个，先断开再重试一次\n", ret);

      argv[0] = "wapi";
      argv[1] = "disconnect";
      argv[2] = "wlan0";
      argv[3] = NULL;

      wifi_run_capture(argv, cap, sizeof(cap));   /* 只为等它跑完，输出丢弃 */

      usleep(800 * 1000);

      argv[0] = "wapi";
      argv[1] = "scan";
      argv[2] = "wlan0";
      argv[3] = NULL;

      if (wifi_run_capture(argv, cap, sizeof(cap)) >= 0)
        {
          ret = wifi_parse_scan(cap, aps, max);
        }
    }

  /* 扫描会把数据面打断（见 wifi_reassoc_saved 注释），扫完立刻接回来 */

  if (g_ssid[0] != '\0')
    {
      wifi_reassoc_saved();
    }
  return ret;
}



int wifi_manager_scan(wifi_ap_t *aps, int max)
{
  int ret;

  pthread_mutex_lock(&g_op_lock);
  ret = wifi_scan_locked(aps, max);
  pthread_mutex_unlock(&g_op_lock);

  return ret;
}
