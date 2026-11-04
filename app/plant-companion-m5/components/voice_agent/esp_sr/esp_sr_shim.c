/****************************************************************************
 * esp_sr_shim.c — ESP-IDF 运行时接口的 NuttX 适配层
 *
 * esp_sr 预编译库（libnsnet.a / libdl_lib.a / libhufzip.a）内部引用了
 * 若干 ESP-IDF 专有接口。本文件在 NuttX 上提供等价实现：
 *
 *   heap_caps_malloc/calloc/free  — ESP-IDF capability 内存分配。
 *     关键：SPIRAM 请求（caps & 0x400，dl_lib 模型/张量用）必须路由到
 *     **PSRAM 独立 heap**（mm_initialize 建私有 heap，见下）。若退化为
 *     普通 malloc 会吃掉内部 SRAM：①337KB 模型在内部 SRAM（~170KB）
 *     直接分配失败 → nsnet create 失败；②挤占 DMA 缓冲 → I2S 数据
 *     满幅/卡死。PSRAM heap 同时满足"模型不占内部 RAM"（用户要求）。
 *   Cache_Start_DCache_Preload 等 — ESP32-S3 DCache 预取（NuttX 无对应，
 *                                   空实现 = 仅损失少量性能）
 *   xQueueCreateMutex 等 FreeRTOS — hufzip 模型加载用的互斥锁
 *
 * 注：esp_log_write/esp_log_timestamp 由 NuttX esp-hal 的
 * esp-hal-3rdparty/components/log 提供；dotproduct_int16 由
 * libdl_lib.a 的 esp32s3_dsp.S.obj 提供 —— 均不再重复定义。
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mm/mm.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <sys/types.h>
#include <time.h>

/* PSRAM 可分配虚地址区间（arch 层 esp32s3_spiram.c，flat build 直接链接）*/
extern uint32_t esp_spiram_allocable_vaddr_start(void);
extern uint32_t esp_spiram_allocable_vaddr_end(void);

/* ROM 提供的 MMU 映射函数（0x400019b0），esp_spiram_init_cache 用它 */
extern int cache_dbus_mmu_set(uint32_t ext_ram, uint32_t vaddr,
                              uint32_t paddr, uint32_t psize,
                              uint32_t num, uint32_t fixed);

/* 2026-08-25：DCache 暂停/恢复（改 MMU 表必须包在中间，否则破坏
 * DCache 状态 → I2S DMA 挂）。esp32s3_himem.c 同款 extern。 */
extern uint32_t cache_suspend_dcache(void);
extern void cache_resume_dcache(uint32_t val);

/* esp-hal 的日志接口（components/log），用于静音 model_create 的海量
 * ESP_LOGI 输出 —— USB-Serial-JTAG FIFO 满时 printf 阻塞主线程（假卡死） */
typedef int (*vprintf_like_t)(const char *fmt, va_list ap);
extern vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
extern void esp_log_level_set(const char *tag, int level);

static int esp_sr_log_noop(const char *fmt, va_list ap)
{
  return 0;
}

void esp_sr_log_silence(void)
{
  esp_log_set_vprintf(esp_sr_log_noop);
  esp_log_level_set("*", 1);  /* ESP_LOG_ERROR 及以上，进一步减少 */
}

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MALLOC_CAP_SPIRAM  (1 << 10)  /* 0x400，与 ESP-IDF 一致 */

#define MMU_ACCESS_SPIRAM  0x8000u    /* BIT(15)，ext_mem_defs.h */

/* ⚠️ NuttX esp32s3 的 mmu_valid_space() 实测返回 0（MMU 表未建 PSRAM
 * 条目）→ esp_spiram_allocable_vaddr 区间为 0 → PSRAM 从未映射。
 * 这里手动映射：DRAM0 cache 空间 0x3c000000-0x3e000000（32MB），
 * flash DROM 占 0x3c000000-0x3d000000（16MB），PSRAM 映射到剩余区
 * 0x3d000000 起 8MB（0x3d000000-0x3d800000）。 */
#define PSRAM_MAP_VADDR    0x3d000000u
#define PSRAM_MAP_SIZE     0x800000u  /* 8MB */

/****************************************************************************
 * PSRAM 私有 heap（模型/张量专用，不占内部 SRAM）
 ****************************************************************************/

static struct mm_heap_s *g_psram_heap = NULL;
static uintptr_t g_psram_start = 0;
static uintptr_t g_psram_end = 0;

/* 2026-08-25：PSRAM 映射必须【提前】到 I2S DMA 配置之前！
 * ⚠️ 根因：原来在 ai_ns_init（record 内，hal_i2s_init 之后）才 lazy 映射
 * PSRAM（cache_dbus_mmu_set 改 MMU 表）→ 运行中改 MMU 破坏 I2S DMA
 * 中断 → RX 3s 超时（r=-110）。voice_agent_init 在 hal_i2s_init 前调用
 * 本函数完成映射，I2S DMA 配置时 PSRAM 已就绪，互不干扰。 */

int esp_sr_psram_early_init(void)
{
  if (g_psram_heap != NULL)
    {
      return 0;
    }

  g_psram_start = esp_spiram_allocable_vaddr_start();
  g_psram_end = esp_spiram_allocable_vaddr_end();

  if (g_psram_end <= g_psram_start)
    {
      /* ① 优先用 NuttX 标准 PSRAM 初始化（物理+MMU+cache 保护+allocable
       * vaddr 管理）——与 BOOT_INIT 同款，但 BOOT_INIT 在 esp32s3 上实测
       * 未生效（allocable vaddr=0），此处显式再调一次标准函数。
       * 2026-08-25 回答用户：能用人家标准包就不手写，手写只作 fallback。 */

      extern int esp_spiram_init(void);
      extern void esp_spiram_init_cache(void);
      extern bool esp_spiram_is_initialized(void);

      if (!esp_spiram_is_initialized())
        {
          if (esp_spiram_init() != OK)
            {
              printf("[esp_sr] esp_spiram_init failed\n");
            }
          else
            {
              esp_spiram_init_cache();
            }
        }
      else
        {
          /* 物理已初始化但 vaddr 仍 0 → init_cache 重跑（mmu_valid_space） */

          esp_spiram_init_cache();
        }

      g_psram_start = esp_spiram_allocable_vaddr_start();
      g_psram_end = esp_spiram_allocable_vaddr_end();
      printf("[esp_sr] NuttX std PSRAM vaddr: 0x%lx-0x%lx\n",
             (unsigned long)g_psram_start, (unsigned long)g_psram_end);
    }

  if (g_psram_end <= g_psram_start)
    {
      /* ② fallback：NuttX 标准路径仍不可用 → 手动映射 8MB 到 flash 之后
       * 的 DRAM0 cache 区。
       * ⚠️ 2026-08-25 修复：必须【暂停 DCache】再改 MMU 表！
       * 原实现直接 cache_dbus_mmu_set（DCache 活动时改 MMU）→ 破坏
       * DCache 状态 → I2S DMA 挂（RX 3s 超时）。NuttX esp32s3_spiram.c
       * 的 esp_spiram_init_cache 同样用 cache_suspend/resume_dcache
       * 包裹（L455-465），照抄该顺序。 */

      uint32_t cache_state;

      printf("[esp_sr] PSRAM not mapped, map 8MB @0x%x...\n",
             (unsigned)PSRAM_MAP_VADDR);
      cache_state = cache_suspend_dcache();
      cache_dbus_mmu_set(MMU_ACCESS_SPIRAM, PSRAM_MAP_VADDR, 0, 64,
                         PSRAM_MAP_SIZE >> 16, 0);
      cache_resume_dcache(cache_state);
      g_psram_start = PSRAM_MAP_VADDR;
      g_psram_end = PSRAM_MAP_VADDR + PSRAM_MAP_SIZE;
    }

  /* 读写自检：确认 PSRAM 映射可访问（否则退化为内部 SRAM） */

  {
    volatile uint32_t *tp = (volatile uint32_t *)g_psram_start;

    *tp = 0x12345678u;
    uint32_t rd = *tp;
    printf("[esp_sr] PSRAM vaddr: 0x%lx-0x%lx self-test: %s\n",
           (unsigned long)g_psram_start, (unsigned long)g_psram_end,
           (rd == 0x12345678u) ? "OK" : "FAIL");

    if (rd != 0x12345678u)
      {
        printf("[esp_sr] PSRAM unusable, fallback to internal SRAM\n");
        return -1;
      }
  }

  g_psram_heap = mm_initialize("psram", (void *)g_psram_start,
                               g_psram_end - g_psram_start);
  printf("[esp_sr] PSRAM heap ready: %p (%lu KB)\n",
         (void *)g_psram_heap,
         (unsigned long)((g_psram_end - g_psram_start) / 1024));

  return (g_psram_heap != NULL) ? 0 : -1;
}

static struct mm_heap_s *psram_heap_get(void)
{
  if (g_psram_heap == NULL)
    {
      esp_sr_psram_early_init();
    }

  return g_psram_heap;
}

/****************************************************************************
 * heap_caps — ESP-IDF capability 内存分配
 ****************************************************************************/

void *heap_caps_malloc(size_t size, uint32_t caps)
{
  if (caps & MALLOC_CAP_SPIRAM)
    {
      struct mm_heap_s *h = psram_heap_get();

      if (h != NULL)
        {
          return mm_malloc(h, size);
        }

      printf("[esp_sr] SPIRAM: no psram heap, fallback malloc(%u)\n",
             (unsigned)size);
    }

  return malloc(size);
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
  void *p = heap_caps_malloc(n * size, caps);

  if (p != NULL)
    {
      memset(p, 0, n * size);
    }

  return p;
}

void heap_caps_free(void *ptr)
{
  if (ptr != NULL && g_psram_heap != NULL &&
      (uintptr_t)ptr >= g_psram_start && (uintptr_t)ptr < g_psram_end)
    {
      mm_free(g_psram_heap, ptr);
    }
  else
    {
      free(ptr);
    }
}

/****************************************************************************
 * Cache 预取 — NuttX 无对应 API，空实现（性能略降，不影响正确性）
 ****************************************************************************/

void Cache_Start_DCache_Preload(uint32_t addr, uint32_t size)
{
}

void Cache_DCache_Preload_Done(void)
{
}

/****************************************************************************
 * FreeRTOS 互斥原语 — hufzip 模型加载用（NuttX 信号量实现）
 ****************************************************************************/

typedef void *QueueHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;

#define portMAX_DELAY 0xffffffffUL

QueueHandle_t xQueueCreateMutex(uint8_t ucQueueType)
{
  sem_t *s = (sem_t *)malloc(sizeof(sem_t));

  if (s != NULL)
    {
      sem_init(s, 0, 1);
    }

  return (QueueHandle_t)s;
}

BaseType_t xQueueSemaphoreTake(QueueHandle_t xQueue, TickType_t xTicksToWait)
{
  sem_t *s = (sem_t *)xQueue;

  if (xTicksToWait == portMAX_DELAY)
    {
      return sem_wait(s) == 0 ? 1 : 0;
    }

  /* 有限超时：用 timedwait（NuttX 时间单位 ns）*/
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += (long)xTicksToWait * 1000000L;
  ts.tv_sec += ts.tv_nsec / 1000000000L;
  ts.tv_nsec %= 1000000000L;
  return sem_timedwait(s, &ts) == 0 ? 1 : 0;
}

BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *pvItemToQueue,
                             TickType_t xTicksToWait, BaseType_t xCopyPosition)
{
  sem_post((sem_t *)xQueue);
  return 1;
}

void vQueueDelete(QueueHandle_t xQueue)
{
  sem_t *s = (sem_t *)xQueue;

  sem_destroy(s);
  free(s);
}
