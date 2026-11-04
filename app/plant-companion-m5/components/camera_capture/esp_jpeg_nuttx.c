/* NuttX allocation/logging adapter for the official JPEG mono-task API.
 * The optional vendor FreeRTOS worker path is intentionally unsupported.
 * Its hooks fail closed and are counted; successful encoding requires zero
 * calls. Existing strong speech-runtime heap/semaphore symbols take priority.
 */
#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

static _Atomic unsigned g_unsupported;
unsigned camera_jpeg_rtos_calls(void){return g_unsupported;}
#define JPEG_WEAK __attribute__((weak))
JPEG_WEAK void *heap_caps_aligned_alloc(size_t alignment,size_t size,uint32_t caps)
{(void)caps;return memalign(alignment,size);}
JPEG_WEAK void *heap_caps_calloc(size_t count,size_t size,uint32_t caps)
{(void)caps;return calloc(count,size);}
JPEG_WEAK void heap_caps_free(void *p){free(p);}
void *heap_caps_aligned_calloc(size_t alignment,size_t count,size_t size,uint32_t caps)
{
  if(size && count>SIZE_MAX/size)return NULL;
  void *p=heap_caps_aligned_alloc(alignment,count*size,caps);
  if(p)memset(p,0,count*size);
  return p;
}
void *heap_caps_calloc_prefer(size_t count,size_t size,size_t choices,...)
{
  if(size && count>SIZE_MAX/size)return NULL;
  va_list args;va_start(args,choices);void *p=NULL;
  for(size_t i=0;i<choices;i++)
    {uint32_t caps=va_arg(args,uint32_t);p=heap_caps_calloc(count,size,caps);if(p)break;}
  va_end(args);return p;
}
/* esp_log_write/timestamp are already provided by NuttX's ESP HAL. */
JPEG_WEAK void *xQueueGenericCreate(unsigned length,unsigned size,uint8_t type)
{(void)length;(void)size;(void)type;g_unsupported++;return NULL;}
JPEG_WEAK int xQueueReceive(void *queue,void *output,uint32_t ticks)
{(void)queue;(void)output;(void)ticks;g_unsupported++;return 0;}
JPEG_WEAK int xQueueGenericSend(void *queue,const void *item,uint32_t ticks,int position)
{(void)queue;(void)item;(void)ticks;(void)position;g_unsupported++;return 0;}
JPEG_WEAK void vQueueDelete(void *queue){(void)queue;g_unsupported++;}
JPEG_WEAK void vTaskDelete(void *task){(void)task;g_unsupported++;}
JPEG_WEAK int xTaskCreatePinnedToCore(void (*entry)(void *),const char *name,
                                    uint32_t stack,void *arg,unsigned priority,
                                    void **handle,int core)
{
  (void)entry;(void)name;(void)stack;(void)arg;(void)priority;(void)core;
  if(handle)*handle=NULL;g_unsupported++;return 0;
}
