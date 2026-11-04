#include <nuttx/config.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <semaphore.h>
#include <time.h>
#include <limits.h>
#include <stdbool.h>
extern void *xtensa_imm_malloc(size_t size);
extern void xtensa_imm_free(void *p);
extern bool xtensa_imm_heapmember(void *p);
void *heap_caps_malloc(size_t size,uint32_t caps)
{
  if ((caps & (1u<<11)) && !(caps & (1u<<10))) return xtensa_imm_malloc(size);
  return malloc(size);
}
void *heap_caps_calloc(size_t n,size_t size,uint32_t caps)
{
  if (size && n>SIZE_MAX/size) return NULL;
  void *p=heap_caps_malloc(n*size,caps);
  if(p)memset(p,0,n*size);
  return p;
}
void heap_caps_free(void *p)
{
  if(p && xtensa_imm_heapmember(p))xtensa_imm_free(p);else free(p);
}
/* Cache_Start_DCache_Preload and Cache_DCache_Preload_Done resolve to
 * the ESP32-S3 ROM exports in esp32s3.rom.ld. Use real asynchronous
 * prefetch instead of the legacy no-op adapter. */

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

void *heap_caps_aligned_alloc(size_t align,size_t size,uint32_t caps)
{
  if((caps&(1u<<11)) && !(caps&(1u<<10)))
    {void *p=xtensa_imm_malloc(size);if(p && (uintptr_t)p%align){xtensa_imm_free(p);return NULL;}return p;}
  return memalign(align,size);
}
