/****************************************************************************
 * apps/plant-companion/services/ai_service.c
 *
 * AI 业务服务：ai_engine 的异步封装
 *
 * 线程模型：
 *   - 每次 analyze_async 创建一个一次性 worker 线程（pthread detached）
 *   - worker 里调 ai_engine_run（阻塞 HTTP），完成后触发回调
 *   - busy 标志防重入；回调由调用方负责投递到 ui_task
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "ai_service.h"
#include "server_bridge.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_busy;
static bool g_inited;

/* worker 请求参数（堆上传递；jpeg 默认拷贝，borrowed=借用不拷贝） */

struct ai_job_s
{
  int  req_type;
  uint8_t *jpeg;          /* 图像数据（默认拷贝；borrowed 时不拷贝） */
  size_t jpeg_len;
  bool borrowed;          /* ⚠️ 3C-2 零拷贝：帧由调用方保证存活（冻结
                           * 的 rxbuf），worker 结束后不 free */
  struct soil_data_s soil;
  bool has_soil;
  ai_service_done_cb_t cb;
  void *user_data;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void *ai_worker(void *arg)
{
  struct ai_job_s *job = (struct ai_job_s *)arg;
  struct ai_service_result_s result;
  int ret;

  memset(&result, 0, sizeof(result));
  result.req_type = job->req_type;
  result.success = false;

  /* 协议 v2：设备端无任何大模型密钥。图像诊断 = RGB565 帧上传服务器
   * → /media/image 拿 media_id → /ai/diagnose 结构化 result（薄端只
   * 渲染，判断全部在服务器）。服务器失败时降级本地引擎（离线演示）。 */

  if (job->req_type == AI_REQ_DIAGNOSE &&
      job->jpeg != NULL && job->jpeg_len > 0)
    {
      struct sb_diag_result_s diag;

      ret = server_bridge_image_diagnose(job->jpeg, job->jpeg_len, &diag);
      if (ret == 0 && diag.ok)
        {
          result.diag = diag;
          result.success = true;
          result.from_server = true;
          result.server_err = 0;

          if (diag.summary[0] != '\0')
            {
              snprintf(result.server_text, sizeof(result.server_text), "%s",
                       diag.summary);
            }
          else
            {
              snprintf(result.server_text, sizeof(result.server_text),
                       "%s 健康 %d 分", diag.name, diag.health_score);
            }

          printf("[AI-Service] 服务器诊断: %s / 健康%d / 匹配%d\n",
                 diag.name, diag.health_score, diag.match);
        }
      else
        {
          result.server_err = ret;
          printf("[AI-Service] 服务器诊断失败(%d)，降级本地引擎\n", ret);
        }
    }

  /* 服务器失败 / 无帧：本地引擎兜底（离线可演示） */

  if (!result.success)
    {
      ret = ai_engine_run(job->has_soil ? &job->soil : NULL,
                          job->jpeg, job->jpeg_len,
                          &result.engine);
      result.success = (ret == 0);
    }

  if (job->cb != NULL)
    {
      job->cb(job->user_data, &result);
    }

  if (job->jpeg != NULL && !job->borrowed)
    {
      free(job->jpeg);
    }

  free(job);

  pthread_mutex_lock(&g_lock);
  g_busy = false;
  pthread_mutex_unlock(&g_lock);

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_service_init(void)
{
  int ret;

  pthread_mutex_lock(&g_lock);
  if (g_inited)
    {
      pthread_mutex_unlock(&g_lock);
      return 0;
    }

  ret = ai_engine_init();
  if (ret == 0)
    {
      g_inited = true;
    }

  pthread_mutex_unlock(&g_lock);
  return ret;
}

void ai_service_deinit(void)
{
  pthread_mutex_lock(&g_lock);
  if (g_inited)
    {
      ai_engine_deinit();
      g_inited = false;
    }

  pthread_mutex_unlock(&g_lock);
}

bool ai_service_busy(void)
{
  bool busy;

  pthread_mutex_lock(&g_lock);
  busy = g_busy;
  pthread_mutex_unlock(&g_lock);

  return busy;
}

int ai_service_analyze_async(const uint8_t *jpeg_data, size_t jpeg_len,
                             bool borrowed,
                             const struct soil_data_s *soil,
                             ai_service_done_cb_t cb, void *user_data)
{
  struct ai_job_s *job;
  pthread_t tid;
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_lock);
  if (g_busy)
    {
      pthread_mutex_unlock(&g_lock);
      return -EBUSY;
    }

  g_busy = true;
  pthread_mutex_unlock(&g_lock);

  job = (struct ai_job_s *)calloc(1, sizeof(struct ai_job_s));
  if (job == NULL)
    {
      pthread_mutex_lock(&g_lock);
      g_busy = false;
      pthread_mutex_unlock(&g_lock);
      return -ENOMEM;
    }

  job->req_type = AI_REQ_DIAGNOSE;
  job->cb = cb;
  job->user_data = user_data;

  if (soil != NULL)
    {
      job->soil = *soil;
      job->has_soil = true;
    }

  if (jpeg_data != NULL && jpeg_len > 0)
    {
      if (borrowed)
        {
          /* ⚠️ 3C-2 零拷贝：不 malloc，借用调用方帧（如冻结的 rxbuf）。
           * 调用方必须保证指针在 worker 运行期间存活（freeze 后
           * rxbuf 不再被预览线程写）。 */
          job->jpeg = (uint8_t *)jpeg_data;
          job->jpeg_len = jpeg_len;
          job->borrowed = true;
        }
      else
        {
          job->jpeg = (uint8_t *)malloc(jpeg_len);
          if (job->jpeg == NULL)
            {
              free(job);
              pthread_mutex_lock(&g_lock);
              g_busy = false;
              pthread_mutex_unlock(&g_lock);
              return -ENOMEM;
            }

          memcpy(job->jpeg, jpeg_data, jpeg_len);
          job->jpeg_len = jpeg_len;
        }
    }

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  /* ⚠️ 协议 v2：worker 栈加大到 16KB —— 结构化诊断响应解析走静态缓冲，
   * 但 result 结构体（server_text 1KB + diag ~1.1KB + 本地引擎结果）整体
   * 在栈上；默认 4KB/旧 8KB 都有溢出风险 → 栈溢出破坏内存（黑屏/死机）。 */

  pthread_attr_setstacksize(&attr, 16384);

  ret = pthread_create(&tid, &attr, ai_worker, job);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      if (job->jpeg != NULL)
        {
          free(job->jpeg);
        }

      free(job);
      pthread_mutex_lock(&g_lock);
      g_busy = false;
      pthread_mutex_unlock(&g_lock);
      return -ret;
    }

  return 0;
}
