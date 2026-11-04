/* Private NSNet allocation accounting. Never interposes the wake engine.
 * Call begin/end only while the existing NN diagnostic gate is owned.
 * This measures demand; it does not make SDK model_create OOM-safe.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

extern void *heap_caps_malloc(size_t, uint32_t);
extern void heap_caps_free(void *);

struct nn_alloc_header { void *base; size_t size; size_t charged; uint32_t magic; };
#define NN_MAGIC 0x4e4e414cu
static size_t live_bytes, peak_bytes, live_blocks, calls, failures;
static size_t charged_bytes,peak_charged,peak_blocks;
static uint32_t used_caps;

int plant_nn_alloc_begin(void)
{
  if (live_blocks) return -EBUSY;
  live_bytes=peak_bytes=calls=failures=0;
  charged_bytes=peak_charged=peak_blocks=0;used_caps=0;
  return 0;
}

static void *allocate(size_t n, uint32_t caps, int use_caps)
{
  calls++;
  if (n>SIZE_MAX-sizeof(struct nn_alloc_header)-15u)
    {failures++;return NULL;}
  size_t total=n+sizeof(struct nn_alloc_header)+15u;
  void *base=use_caps?heap_caps_malloc(total,caps):malloc(total);
  if (!base) {failures++;return NULL;}
  uintptr_t aligned=((uintptr_t)base+sizeof(struct nn_alloc_header)+15u)&~(uintptr_t)15u;
  struct nn_alloc_header *h=(struct nn_alloc_header *)aligned-1;
  h->base=base;h->size=n;h->charged=total;h->magic=NN_MAGIC;
  charged_bytes+=total;if(charged_bytes>peak_charged)peak_charged=charged_bytes;
  if(use_caps)used_caps|=caps;
  live_blocks++;if(live_blocks>peak_blocks)peak_blocks=live_blocks;live_bytes+=n;if(live_bytes>peak_bytes)peak_bytes=live_bytes;
  return (void *)aligned;
}

void *plant_nn_port_malloc(size_t n) {return allocate(n,0,0);}
void *plant_nn_port_heap_caps_malloc(size_t n,uint32_t caps)
{return allocate(n,caps,1);}
static void *allocate_zero(size_t n,size_t size,uint32_t caps,int use_caps)
{
  if(size && n>SIZE_MAX/size) {calls++;failures++;return NULL;}
  void *p=allocate(n*size,caps,use_caps);if(p)memset(p,0,n*size);return p;
}
void *plant_nn_port_calloc(size_t n,size_t size)
{return allocate_zero(n,size,0,0);}
void *plant_nn_port_heap_caps_calloc(size_t n,size_t size,uint32_t caps)
{return allocate_zero(n,size,caps,1);}
void plant_nn_port_free(void *p)
{
  if(!p)return;
  struct nn_alloc_header *h=(struct nn_alloc_header *)p-1;
  if(h->magic!=NN_MAGIC)abort();
  void *base=h->base;size_t n=h->size;h->magic=0;
  live_bytes-=n;charged_bytes-=h->charged;live_blocks--;
  /* Dispatches correctly for both normal RAM and internal-capability RAM. */
  heap_caps_free(base);
}
int plant_nn_alloc_report(void)
{
  printf("[NNAlloc] calls=%lu live_blocks=%lu live_bytes=%lu peak_bytes=%lu failed=%lu charged_peak=%lu peak_blocks=%lu caps=%lu\n",
    (unsigned long)calls,(unsigned long)live_blocks,(unsigned long)live_bytes,
    (unsigned long)peak_bytes,(unsigned long)failures,(unsigned long)peak_charged,
    (unsigned long)peak_blocks,(unsigned long)used_caps);
  return live_blocks||failures?-EIO:0;
}
