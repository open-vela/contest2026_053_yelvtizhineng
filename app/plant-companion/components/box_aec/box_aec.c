/* Standalone ESP-SR AEC; runs inline on the existing capture owner. */
#include "box_aec.h"
#include "esp_aec.h"
#include "../../hal/hal_i2s.h"
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#ifndef BOX_AEC_HOST_TEST
extern void xtensa_coproc_enable(int cp);
#else
static void xtensa_coproc_enable(int cp){(void)cp;}
#endif
#define AEC_LIMIT 1024
#define DELIVERY_SAMPLES 2004
struct aec_buffers
{
  int16_t mic[AEC_LIMIT] __attribute__((aligned(16)));
  int16_t ref[AEC_LIMIT] __attribute__((aligned(16)));
  int16_t out[AEC_LIMIT] __attribute__((aligned(16)));
  int16_t raw[AEC_LIMIT] __attribute__((aligned(16)));
  int16_t delivery[DELIVERY_SAMPLES];
};
static struct aec_buffers *s_buf;
static aec_handle_t *s_aec;
static int s_chunk,s_fill;
static bool s_attempted;
static bool s_speaker_active;
void box_aec_set_speaker_active(bool active)
{
  if(active && !s_speaker_active && s_aec)
    {
      /* Each playback turn starts at a fresh reference epoch. Do not
       * advance the stateful filter with invented silence across gaps. */
      xtensa_coproc_enable(1<<3);
      aec_destroy(s_aec);
      s_aec=aec_create(16000,4,1,AEC_MODE_VOIP_HIGH_PERF);
      s_chunk=s_aec?aec_get_chunksize(s_aec):0;
      if(s_chunk<1||s_chunk>AEC_LIMIT){if(s_aec)aec_destroy(s_aec);s_aec=NULL;}
      s_fill=0;
      if(!s_aec)printf("[AEC] playback reset unavailable; ordinary voice retained\n");
    }
  if(!active && s_speaker_active)s_fill=0;
  s_speaker_active=active;
}
static uint64_t s_mic_energy,s_ref_energy,s_out_energy;
static uint32_t s_samples,s_frames,s_active_frames;
static uint64_t s_process_cycles;
static uint32_t s_process_frames;
static uint32_t aec_cycles(void)
{
#ifdef __XTENSA__
  uint32_t value;__asm__ __volatile__("rsr %0, ccount":"=a"(value));return value;
#else
  return 0;
#endif
}
static int s_echo_tail;
static size_t s_delivery_count;
bool box_aec_ready(void){return s_aec!=NULL;}
bool box_aec_init(void)
{
  if(s_attempted)return box_aec_ready();
  s_attempted=true;
  s_buf=memalign(16,sizeof(*s_buf));
  if(!s_buf)return false;
  xtensa_coproc_enable(1<<3);
  s_aec=aec_create(16000,4,1,AEC_MODE_VOIP_HIGH_PERF);
  s_chunk=s_aec?aec_get_chunksize(s_aec):0;
  if(!s_aec || s_chunk<1 || s_chunk>AEC_LIMIT || hal_i2s_reference_enable()<0)
    {
      if(s_aec)aec_destroy(s_aec);
      s_aec=NULL;free(s_buf);s_buf=NULL;
      printf("[AEC] unavailable; ordinary voice interaction retained\n");
      return false;
    }
  printf("[AEC] ready sample_rate=16000 chunk=%d filter=4 mode=4 reference=%s\n",s_chunk,hal_i2s_reference_is_hardware()?"adc3":"software");
  return true;
}
void box_aec_begin(void){s_fill=0;s_delivery_count=0;}
bool box_aec_speaker_active(void){return s_speaker_active;}
void box_aec_record_begin(void)
{
  /* A logical utterance boundary is not a discontinuity in the microphone
   * or reference clocks. Carry the incomplete DSP frame into the next one. */
  if(!s_speaker_active)box_aec_begin();
  else s_delivery_count=0;
}
void box_aec_record_end(box_aec_sink sink,void *arg)
{
  if(!s_speaker_active){box_aec_finish(sink,arg);return;}
  /* Deliver completed frames without feeding artificial silence into the
   * stateful filter or discarding its delayed near-end phonemes. */
  if(s_aec&&s_delivery_count){sink(s_buf->delivery,s_delivery_count,arg);s_delivery_count=0;}
}
static void process(int valid,box_aec_sink sink,void *arg)
{
  bool active=false;
  for(int i=0;i<valid;i++)if(s_buf->ref[i]){active=true;break;}
  if(active)s_echo_tail=(3200+s_chunk-1)/s_chunk;
  else if(s_echo_tail){s_echo_tail--;active=true;}
  /* No playback reference: preserve near-end speech rather than applying
   * AEC's mutable preprocessing to a quiet child's wake word. */
  /* Advance the stateful filter through zero-reference speech pauses.
   * Stopping the DSP clock here delays and fragments the next syllables.
   * The vendor may modify its input, so retain an independent raw bypass. */
  memcpy(s_buf->raw,s_buf->mic,(size_t)s_chunk*2);
  if(active && s_speaker_active)for(int i=0;i<valid;i++)
    {
      int32_t m=s_buf->mic[i],r=s_buf->ref[i];
      s_mic_energy+=(uint64_t)(m*m);s_ref_energy+=(uint64_t)(r*r);
    }
  if(s_speaker_active)
    {
      xtensa_coproc_enable(1<<3);
      uint32_t began=aec_cycles();
      aec_process(s_aec,s_buf->mic,s_buf->ref,s_buf->out);
      s_process_cycles+=(uint32_t)(aec_cycles()-began);s_process_frames++;
      if(active)
        {
          for(int i=0;i<valid;i++){int32_t value=s_buf->out[i];s_out_energy+=(uint64_t)(value*value);}
          s_samples+=valid;s_active_frames++;
        }
    }
  const int16_t *pcm=s_speaker_active?s_buf->out:s_buf->raw;
  s_frames++;s_fill=0;
  /* Keep the original collector's energy gate/pre-roll callback cadence.
   * AEC's 32ms frames must not silently change utterance detection. */
  size_t left=(size_t)valid;
  while(left)
    {
      size_t take=DELIVERY_SAMPLES-s_delivery_count;if(take>left)take=left;
      memcpy(s_buf->delivery+s_delivery_count,pcm,take*2);
      s_delivery_count+=take;pcm+=take;left-=take;
      if(s_delivery_count==DELIVERY_SAMPLES)
        {sink(s_buf->delivery,s_delivery_count,arg);s_delivery_count=0;}
    }
}
void box_aec_feed(const int16_t *mic,const int16_t *ref,size_t n,box_aec_sink sink,void *arg)
{
  if(!s_aec){sink(mic,n,arg);return;}
  while(n)
    {
      size_t take=(size_t)(s_chunk-s_fill);if(take>n)take=n;
      memcpy(s_buf->mic+s_fill,mic,take*2);memcpy(s_buf->ref+s_fill,ref,take*2);
      s_fill+=(int)take;mic+=take;ref+=take;n-=take;
      if(s_fill==s_chunk)process(s_chunk,sink,arg);
    }
}
void box_aec_finish(box_aec_sink sink,void *arg)
{
  if(s_aec && s_fill)
    {
      int valid=s_fill;
      memset(s_buf->mic+s_fill,0,(s_chunk-s_fill)*2);
      memset(s_buf->ref+s_fill,0,(s_chunk-s_fill)*2);
      process(valid,sink,arg);
    }
  if(s_aec && s_delivery_count)
    {sink(s_buf->delivery,s_delivery_count,arg);s_delivery_count=0;}
}
void box_aec_dump(void)
{
  uint32_t n=s_samples?s_samples:1;
  printf("[AEC] enabled=%d frames=%lu active=%lu samples=%lu mic_mse=%lu ref_mse=%lu out_mse=%lu\n",
    s_aec!=NULL,(unsigned long)s_frames,(unsigned long)s_active_frames,(unsigned long)s_samples,
    (unsigned long)(s_mic_energy/n),(unsigned long)(s_ref_energy/n),(unsigned long)(s_out_energy/n));
  printf("[AEC] process_mean_us=%lu frame_samples=%d\n",(unsigned long)(s_process_cycles/240/(s_process_frames?s_process_frames:1)),s_chunk);
}
/* AEC data are CPU-only. Its private allocator bindings keep PSRAM buffers
 * out of the small internal DMA/Wi-Fi heap; other SDK allocations unchanged. */
void *box_aec_caps_calloc(size_t n,size_t bytes,uint32_t caps)
{
  (void)caps;if(bytes && n>SIZE_MAX/bytes)return NULL;
  size_t total=n*bytes;void *p=memalign(16,total);if(p)memset(p,0,total);return p;
}
void *box_aec_dl_calloc(int n,int bytes,int align)
{
  if(n<0 || bytes<0 || (bytes && (size_t)n>SIZE_MAX/(size_t)bytes))return NULL;
  size_t total=(size_t)n*(size_t)bytes;
  if(align<16)align=16;
  void *p=memalign((size_t)align,total);if(p)memset(p,0,total);return p;
}
void box_aec_dl_free(void *p){free(p);}
void *box_aec_dl_calloc_psram(int n,int bytes,int align)
{return box_aec_dl_calloc(n,bytes,align);}
/* ESP-DSP sqrt approximation, expressed without pointer-aliasing UB.
 * modules/math/sqrt/float/dsps_sqrt_f32_ansi.c, Apache-2.0. */
float box_aec_sqrt_approx(float f)
{union {float f;uint32_t u;} x={.f=f};x.u=0x1fbb4000u+(x.u>>1);return x.f;}
