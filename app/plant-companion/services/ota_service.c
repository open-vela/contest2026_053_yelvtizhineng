/****************************************************************************
 * apps/plant-companion/services/ota_service.c
 *
 * OTA 升级 UI 服务（阶段 A）：把阻塞的 ota_check_version / ota_check_update
 * 放进后台 worker 线程，共享结果供 screen_ota 定时器轮询。
 *
 * 约束：
 *   - worker 栈用静态缓冲（WiFi 后堆 ~17KB，不能吃堆；8KB 覆盖
 *     ota_update_common 的 buf[1024] 等局部变量）
 *   - worker 只写共享结构（锁保护），不碰 LVGL 对象
 *   - ota_check_update 内部无取消点：abort 只能丢弃结果，下载无法打断
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "ota_service.h"
#include "../ota/ota.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

#define OTA_SVC_MODE_CHECK   0   /* 仅检查版本（查询服务器） */
#define OTA_SVC_MODE_UPGRADE 1   /* 完整升级（下载+切换+重启） */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ota_ui_result_s g_result;
static volatile bool g_cancel;

/* ⚠️ 2026-09-01 内存回归：worker 栈**不能**用静态 .bss 缓冲（曾与时间
 * worker 4KB 共 12KB 静态栈把堆挤到 free 9.3KB → wapi spawn EPERM →
 * WiFi 连不上）。改为 pthread_attr_setstacksize 从堆分配 8KB 瞬时
 * （OTA 是低频独占流程，不与语音并发，8KB 瞬时可接受）。 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* OTA 错误码 → 中文原因（供 UI 直接显示） */

static const char *ota_err_text(int ret)
{
  switch (ret)
    {
      case OTA_ERR_NETWORK:
        return "无法连接 OTA 服务器";

      case OTA_ERR_NO_UPDATE:
        return "已是最新版本";

      case OTA_ERR_SIZE_EXCEEDED:
        return "固件大小超限";

      case OTA_ERR_FLASH_WRITE:
        return "写入 Flash 失败";

      case OTA_ERR_SHA256:
        return "校验失败（SHA256 不匹配）";

      case OTA_ERR_HARDWARE_ID:
        return "固件硬件不匹配";

      default:
        return "网络错误，请检查连接";
    }
}

static void *ota_worker(void *arg)
{
  int mode = (int)(intptr_t)arg;
  int ret;

  if (mode == OTA_SVC_MODE_CHECK)
    {
      struct ota_version_info_s info;
      int cur_major;
      int cur_minor;
      int cur_patch;

      ota_get_current_version(&cur_major, &cur_minor, &cur_patch);

      pthread_mutex_lock(&g_lock);
      g_result.current_major = cur_major;
      g_result.current_minor = cur_minor;
      g_result.current_patch = cur_patch;
      g_result.state = OTA_UI_CHECKING;
      pthread_mutex_unlock(&g_lock);

      ret = ota_check_version(&info);

      if (g_cancel)
        {
          /* 页面已销毁：结果丢弃 */
        }
      else if (ret == OTA_OK)
        {
          pthread_mutex_lock(&g_lock);
          g_result.state = OTA_UI_AVAILABLE;
          g_result.new_major = (int)info.version_major;
          g_result.new_minor = (int)info.version_minor;
          g_result.new_patch = (int)info.version_patch;
          pthread_mutex_unlock(&g_lock);
        }
      else if (ret == OTA_ERR_NO_UPDATE)
        {
          pthread_mutex_lock(&g_lock);
          g_result.state = OTA_UI_UP_TO_DATE;
          pthread_mutex_unlock(&g_lock);
        }
      else
        {
          pthread_mutex_lock(&g_lock);
          g_result.state = OTA_UI_ERROR;
          snprintf(g_result.error, sizeof(g_result.error), "%s",
                   ota_err_text(ret));
          pthread_mutex_unlock(&g_lock);
        }
    }
  else
    {
      /* 完整升级：阻塞直至重启成功 / 失败返回 */

      pthread_mutex_lock(&g_lock);
      g_result.state = OTA_UI_DOWNLOADING;
      g_result.downloaded = 0;
      g_result.total = 0;
      pthread_mutex_unlock(&g_lock);

      ret = ota_check_update();

      /* 成功时内部 sleep(3)+up_systemreset，不可达；到这说明失败 */

      pthread_mutex_lock(&g_lock);
      g_result.state = OTA_UI_ERROR;
      snprintf(g_result.error, sizeof(g_result.error), "%s (%d)",
               ota_err_text(ret), ret);
      pthread_mutex_unlock(&g_lock);
    }

  pthread_mutex_lock(&g_lock);
  g_result.busy = false;
  g_cancel = false;
  pthread_mutex_unlock(&g_lock);

  return NULL;
}

static int ota_service_start(int mode)
{
  pthread_t tid;
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_lock);
  if (g_result.busy)
    {
      pthread_mutex_unlock(&g_lock);
      return -EBUSY;
    }

  g_result.busy = true;
  g_cancel = false;
  pthread_mutex_unlock(&g_lock);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 8192);   /* 堆分配，不用静态 .bss 栈 */

  ret = pthread_create(&tid, &attr, ota_worker, (void *)(intptr_t)mode);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      pthread_mutex_lock(&g_lock);
      g_result.busy = false;
      pthread_mutex_unlock(&g_lock);
      return -ret;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ota_service_init(void)
{
  pthread_mutex_lock(&g_lock);
  memset(&g_result, 0, sizeof(g_result));
  ota_get_current_version(&g_result.current_major,
                          &g_result.current_minor,
                          &g_result.current_patch);
  pthread_mutex_unlock(&g_lock);
}

int ota_service_check(void)
{
  return ota_service_start(OTA_SVC_MODE_CHECK);
}

int ota_service_upgrade(void)
{
  return ota_service_start(OTA_SVC_MODE_UPGRADE);
}

void ota_service_get(struct ota_ui_result_s *out)
{
  struct ota_status_s st;

  pthread_mutex_lock(&g_lock);
  *out = g_result;
  pthread_mutex_unlock(&g_lock);

  /* 下载中：合并 ota.c 的实时进度（downloaded/total，ota_http_download
   * 逐块更新） */

  if (out->state == OTA_UI_DOWNLOADING && ota_get_status(&st) == 0)
    {
      out->downloaded = st.downloaded;
      out->total = st.total;
    }
}

void ota_service_abort(void)
{
  pthread_mutex_lock(&g_lock);
  g_cancel = true;
  pthread_mutex_unlock(&g_lock);
}
