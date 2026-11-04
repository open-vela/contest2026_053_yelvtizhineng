/* Private bridge for the pinned ESP-SR 2.3.1 FFT error-message paths only.
 * These three callers load a 32-bit stderr pointer at offset 12, then pass
 * it opaquely to fwrite. No caller uses a FILE structure or other _reent
 * fields. The extraction tool verifies the call sites and namespaces both
 * imports. This does not implement or replace NuttX's global newlib ABI.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#ifdef __XTENSA__
#include <malloc.h>
extern struct mallinfo xtensa_imm_mallinfo(void);
#endif

/* Private SDK allocations only. Ordinary firmware allocators are unchanged.
 * The NS diagnostic gate serializes mode changes and the SDK lifetime. */
extern void *xtensa_imm_malloc(size_t);
extern void xtensa_imm_free(void *);
extern bool xtensa_imm_heapmember(void *);
extern void *heap_caps_malloc(size_t,uint32_t);
static bool g_ns_internal;
static unsigned g_ns_live,g_ns_allocs,g_ns_frees,g_ns_failed,g_ns_internal_allocs;
static uint32_t g_ns_caps;
static uint64_t g_ns_bytes;
static size_t g_ns_largest,g_ns_required,g_ns_required_largest;
static unsigned g_ns_required_count;

int plant_ns_alloc_mode(bool internal)
{
  if(g_ns_live)return -EBUSY;
#ifdef __XTENSA__
  if(internal)
    {
      struct mallinfo info=xtensa_imm_mallinfo();
      size_t required=g_ns_required+(size_t)g_ns_required_count*32+8192;
      printf("[NSAlloc] preflight free=%u largest=%u required=%u block=%u\n",
             info.fordblks,info.mxordblk,(unsigned)required,
             (unsigned)g_ns_required_largest);
      if(!g_ns_required)return -EAGAIN;
      if(info.fordblks<required || info.mxordblk<g_ns_required_largest+32)
        return -ENOMEM;
    }
#endif
  g_ns_internal=internal;
  g_ns_allocs=g_ns_frees=g_ns_failed=g_ns_internal_allocs=g_ns_caps=0;
  g_ns_bytes=0;g_ns_largest=0;return 0;
}
static void *ns_allocate(size_t size,uint32_t caps,bool has_caps)
{
  void *p=g_ns_internal?xtensa_imm_malloc(size):
          has_caps?heap_caps_malloc(size,caps):malloc(size);
  if(!p){g_ns_failed++;return NULL;}
  g_ns_live++;g_ns_allocs++;g_ns_bytes+=size;g_ns_caps|=caps;
  if(size>g_ns_largest)g_ns_largest=size;
  if(xtensa_imm_heapmember(p))g_ns_internal_allocs++;
  return p;
}
void *plant_ns_alloc(size_t size){return ns_allocate(size,0,false);}
void *plant_ns_caps_alloc(size_t size,uint32_t caps)
{return ns_allocate(size,caps,true);}
void *plant_ns_calloc(size_t n,size_t size)
{
  if(size && n>SIZE_MAX/size){g_ns_failed++;return NULL;}
  void *p=plant_ns_alloc(n*size);if(p)memset(p,0,n*size);return p;
}
void plant_ns_free(void *p)
{
  if(!p)return;
  if(xtensa_imm_heapmember(p))xtensa_imm_free(p);else free(p);
  g_ns_frees++;if(g_ns_live)g_ns_live--;
}
int plant_ns_alloc_report(void)
{
  printf("[NSAlloc] internal_mode=%d allocs=%u frees=%u live=%u failed=%u bytes=%llu internal_allocs=%u caps=%lu\n",
         g_ns_internal,g_ns_allocs,g_ns_frees,g_ns_live,g_ns_failed,
         (unsigned long long)g_ns_bytes,g_ns_internal_allocs,(unsigned long)g_ns_caps);
  int rc=g_ns_live || g_ns_allocs!=g_ns_frees?-EIO:0;
  if(!rc && !g_ns_internal && !g_ns_failed)
    {g_ns_required=g_ns_bytes;g_ns_required_largest=g_ns_largest;g_ns_required_count=g_ns_allocs;}
  return rc;
}

/* These imports are private to the extracted toolchain math closure. */
int *plant_ns_math_errno(void) { return &errno; }
float plant_ns_math_sqrtf(float x)
{
  if(x==0.0f || (isinf(x) && x>0.0f))return x;
  if(isnan(x))return x+x;
  if(x<0.0f){errno=EDOM;return NAN;}
  return sqrtf(x);
}

static uint32_t g_context_words[4];
static _Atomic unsigned g_error_writes;

void *plant_ns_getreent(void) { return g_context_words; }
unsigned plant_ns_error_writes(void) { return g_error_writes; }

size_t plant_ns_error_fwrite(const void *data, size_t size, size_t count,
                           void *opaque_newlib_stream)
{
  (void)opaque_newlib_stream;
  if (!size || !count || size > SIZE_MAX / count) return 0;
  g_error_writes++;
  return fwrite(data, size, count, stderr);
}

/* NuttX's selected libm lacks log1pf. Correct the rounding of 1+x before
 * taking logf; separate volatile float steps preserve that compensation
 * even if other application files use relaxed floating-point flags.
 * This symbol is private to the pinned NS object, never a global override.
 */
float plant_ns_log1pf(float x)
{
  volatile float sum=1.0f+x;
  float u=sum;
  if(u==1.0f)return x; /* Includes tiny values and signed zero. */
  if(!(u>0.0f) || !isfinite(u))return logf(u);
  volatile float rounded_delta=u-1.0f;
  volatile float correction=rounded_delta-x;
  return logf(u)-correction/u;
}
