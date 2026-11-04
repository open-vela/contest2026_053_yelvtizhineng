/****************************************************************************
 * apps/plant-companion/components/system_monitor/device_cfg.c
 *
 * plant.cfg 读写实现（见 device_cfg.h 头注释）。纯文本、无动态内存：
 * 逐行 fgets 解析；保存先写 .tmp 再 rename，避免半截文件被开机读到。
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

#include "sd_card.h"
#include "device_cfg.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
#  define PLANT_CFG_PATH "/data/plant.cfg"
#  define PLANT_CFG_TMP  "/data/plant.cfg.tmp"
#else
#  define PLANT_CFG_PATH SD_MOUNTPOINT "/plant.cfg"
#  define PLANT_CFG_TMP  SD_MOUNTPOINT "/plant.cfg.tmp"
#endif
static pthread_mutex_t g_cfg_write_lock = PTHREAD_MUTEX_INITIALIZER;

const char *device_cfg_path(void) { return PLANT_CFG_PATH; }

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 复制一个值并去掉行尾 CR/LF（值可能带空格，不做 trim） */

static void cfg_copy_val(const char *val, char *dst, size_t dst_size)
{
  size_t len;

  if (dst == NULL || dst_size == 0)
    {
      return;
    }

  dst[0] = '\0';
  if (val == NULL)
    {
      return;
    }

  len = strnlen(val, dst_size);
  while (len > 0 && (val[len - 1] == '\r' || val[len - 1] == '\n'))
    {
      len--;
    }

  if (len >= dst_size)
    {
      len = dst_size - 1;
    }

  memcpy(dst, val, len);
  dst[len] = '\0';
}

static int cfg_ensure_mounted(void)
{
#ifdef CONFIG_ARCH_BOARD_M5STACK_CORES3
  /* Use the existing board-mounted LittleFS; never format it here. */
  struct stat st;
  if (stat("/data", &st) < 0) return -errno;
  return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
#else
  if (sd_card_status())
    {
      return 0;
    }

  return sd_card_mount();
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 整文件重写 plant.cfg：原来的四个键 + 设备序列号（sn 可为 NULL/空跳过）。
 * 2026-09-22 新增 sn：设备号决定云端身份，多块板卡必须各不相同，
 * 出厂时可以只烧一份固件，再按板写各自的 sn。 */

static int cfg_write_all(const char *ssid, const char *password,
                         const char *host, uint16_t port, const char *sn)
{
  FILE *fp;
  int rc = 0;

  if (cfg_ensure_mounted() < 0)
    {
      return -EIO;
    }

  pthread_mutex_lock(&g_cfg_write_lock);
  fp = fopen(PLANT_CFG_TMP, "w");
  if (fp == NULL)
    {
      rc = -errno;
      pthread_mutex_unlock(&g_cfg_write_lock);
      return rc;
    }

  fprintf(fp, "# plant-companion boot config (auto-generated)\n");
  if (sn != NULL && sn[0] != '\0')
    {
      fprintf(fp, "sn=%s\n", sn);
    }

  if (ssid != NULL && ssid[0] != '\0')
    {
      fprintf(fp, "wifi_ssid=%s\n", ssid);
      fprintf(fp, "wifi_password=%s\n", password != NULL ? password : "");
    }

  if (host != NULL && host[0] != '\0')
    {
      fprintf(fp, "server_host=%s\n", host);
      fprintf(fp, "server_port=%u\n", (unsigned)port);
    }

  if (ferror(fp) || fflush(fp) != 0 || fsync(fileno(fp)) != 0)
    {
      rc = errno ? -errno : -EIO;
    }
  if (fclose(fp) != 0 && rc == 0)
    {
      rc = -errno;
    }

  if (rc == 0 && rename(PLANT_CFG_TMP, PLANT_CFG_PATH) != 0)
    {
      rc = -errno;
    }

  if (rc != 0)
    {
      remove(PLANT_CFG_TMP);
    }

  pthread_mutex_unlock(&g_cfg_write_lock);
  return rc;
}

int device_cfg_load_sn(char *sn, size_t sn_size)
{
  FILE *fp;
  char line[160];

  if (sn != NULL && sn_size > 0)
    {
      sn[0] = '\0';
    }

  if (cfg_ensure_mounted() < 0)
    {
      return -EIO;
    }

  fp = fopen(PLANT_CFG_PATH, "r");
  if (fp == NULL)
    {
      return -errno;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      if (strncmp(line, "sn=", 3) == 0)
        {
          cfg_copy_val(line + 3, sn, sn_size);
          break;
        }
    }

  fclose(fp);
  return (sn != NULL && sn[0] != '\0') ? 0 : -ENOENT;
}

int device_cfg_save_sn(const char *sn)
{
  char ssid[33] = { 0 };
  char password[65] = { 0 };
  char host[128] = { 0 };
  uint16_t port = 0;

  if (sn == NULL || sn[0] == '\0')
    {
      return -EINVAL;
    }

  /* 其余键原样保留：先读回来，再整文件重写 */

  device_cfg_load(ssid, sizeof(ssid), password, sizeof(password),
                  host, sizeof(host), &port);

  return cfg_write_all(ssid, password, host, port, sn);
}

int device_cfg_save(const char *ssid, const char *password,
                    const char *host, uint16_t port)
{
  char sn[40] = { 0 };

  /* 别把已写好的设备号弄丢：读出来一起写回去 */

  device_cfg_load_sn(sn, sizeof(sn));

  return cfg_write_all(ssid, password, host, port, sn);
}

int device_cfg_load(char *ssid, size_t ssid_size,
                    char *password, size_t password_size,
                    char *host, size_t host_size,
                    uint16_t *port)
{
  FILE *fp;
  char line[160];

  if (ssid != NULL && ssid_size > 0)
    {
      ssid[0] = '\0';
    }

  if (password != NULL && password_size > 0)
    {
      password[0] = '\0';
    }

  if (host != NULL && host_size > 0)
    {
      host[0] = '\0';
    }

  if (port != NULL)
    {
      *port = 0;
    }

  if (cfg_ensure_mounted() < 0)
    {
      return -EIO;
    }

  fp = fopen(PLANT_CFG_PATH, "r");
  if (fp == NULL)
    {
      return -errno;   /* 常见 -ENOENT：还没有配置文件 */
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
          continue;
        }

      if (strncmp(line, "wifi_ssid=", 10) == 0)
        {
          cfg_copy_val(line + 10, ssid, ssid_size);
        }
      else if (strncmp(line, "wifi_password=", 14) == 0)
        {
          cfg_copy_val(line + 14, password, password_size);
        }
      else if (strncmp(line, "server_host=", 12) == 0)
        {
          cfg_copy_val(line + 12, host, host_size);
        }
      else if (strncmp(line, "server_port=", 12) == 0)
        {
          if (port != NULL)
            {
              *port = (uint16_t)atoi(line + 12);
            }
        }
    }

  fclose(fp);
  return 0;
}

int device_cfg_clear(void)
{
  if (cfg_ensure_mounted() < 0)
    {
      return -EIO;
    }

  if (remove(PLANT_CFG_PATH) == 0)
    {
      return 0;
    }

  return (errno == ENOENT) ? 0 : -errno;
}

int device_cfg_forget_wifi(void)
{
  char host[128] = {0}, sn[40] = {0}; uint16_t port = 0;
  int rc = device_cfg_load(NULL, 0, NULL, 0, host, sizeof(host), &port);
  if (rc < 0 && rc != -ENOENT) return rc;
  device_cfg_load_sn(sn, sizeof(sn));
  return cfg_write_all(NULL, NULL, host, port, sn);
}
