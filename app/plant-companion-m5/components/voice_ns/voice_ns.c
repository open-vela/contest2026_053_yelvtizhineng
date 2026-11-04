/* Diagnostic side path: official classical NS -> independent VAD.
 * Original PCM and the production onset/endpoint remain untouched.
 * Disabled by default; no NS allocation or processing until explicitly on.
 */
#include "voice_ns.h"
#include "../voice_vad/voice_vad.h"
#include "../voice_vad/voice_onset.h"
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#ifdef __XTENSA__
#include <nuttx/sched.h>
#include "math_vectors.h"
#endif

#define ns_pro_create plant_ns_ns_pro_create
#define ns_process plant_ns_ns_process
#define ns_destroy plant_ns_ns_destroy
#include "vendor/esp_ns.h"

#include "ns_nn_iface.h"
#include "ns_nn_pair.h"
#include "voice_endpoint_policy.h"
#define NS_FRAME 160u
#define NS_INPUT_MAX 2048u
#define NS_OUTPUT_MAX (NS_INPUT_MAX + 512u)

struct voice_ns_probe
{
  ns_handle_t handle;
  struct voice_vad *vad;
  const struct plant_nn_iface *nn_api;
  plant_nn_data_t *nn;
  unsigned frame_samples;bool failed;
  int16_t input[512] __attribute__((aligned(16)));
  int16_t output[512] __attribute__((aligned(16)));
  int16_t clean[NS_OUTPUT_MAX] __attribute__((aligned(16)));
  size_t used;
  uint32_t total, frames, max_us;
  uint64_t total_us;
  int64_t noise;
  unsigned recent;
  bool onset_seen,decision_valid,last_speech;
  unsigned raw_recent,endpoint_blocks,endpoint_differences;
};

static _Atomic bool g_trace;
static _Atomic unsigned g_endpoint_mode; /* off0/shadow1/assist2 */
static _Atomic unsigned g_probe_mode; /* mild0, diagnostic only */
static pthread_mutex_t g_probe_gate=PTHREAD_MUTEX_INITIALIZER;
extern unsigned plant_ns_error_writes(void);
extern int plant_ns_alloc_mode(bool internal);
extern int plant_ns_alloc_report(void);
extern void *plant_ns_esp_kiss_fftr_alloc(int, int, void *, size_t *);
#ifdef __XTENSA__
extern void xtensa_coproc_enable(int);
#endif

void voice_ns_trace_set(bool enabled) { g_trace=enabled; }
bool voice_ns_trace_enabled(void) { return g_trace; }
bool voice_ns_probe_requested(void){return g_trace || g_endpoint_mode!=0;}
unsigned voice_ns_endpoint_get_mode(void){return g_endpoint_mode;}
int voice_ns_endpoint_set_mode(unsigned mode)
{
  if(mode>2)return -EINVAL;
  if(pthread_mutex_trylock(&g_probe_gate)!=0)return -EBUSY;
  g_endpoint_mode=mode;pthread_mutex_unlock(&g_probe_gate);return 0;
}
bool voice_ns_endpoint_vote(struct voice_ns_probe *p,bool waiting,
                            bool raw,bool raw_onset,int32_t mean,int32_t noise)
{
  if(!p || !p->nn)return raw;
  bool valid=p->decision_valid && !p->failed;
  bool candidate=plant_endpoint_speech(&p->raw_recent,raw,mean,noise,valid,p->last_speech);
  unsigned mode=g_endpoint_mode;
  bool selected=mode==2 && !waiting?candidate:raw;
  plant_nn_pair_raw_mark(p,waiting && raw_onset,waiting?raw:candidate);
  if(mode)
    {
      p->endpoint_blocks++;p->endpoint_differences+=selected!=raw;
      if(g_trace)printf("[Endpoint] mode=%u waiting=%d raw=%d nn_valid=%d nn=%d mean=%ld noise=%ld recent=%u candidate=%d selected=%d raw_onset=%d\n",
        mode,waiting,raw,valid,p->last_speech,(long)mean,(long)noise,p->raw_recent,candidate,selected,raw_onset);
    }
  return selected;
}


unsigned voice_ns_probe_get_mode(void) { return g_probe_mode; }
int voice_ns_probe_set_mode(unsigned mode)
{
  if(mode>3)return -EINVAL;
  /* Never switch an active SDK instance underneath the capture worker. */
  if(pthread_mutex_trylock(&g_probe_gate)!=0)return -EBUSY;
  g_probe_mode=mode;
  pthread_mutex_unlock(&g_probe_gate);
  return 0;
}


static uint64_t now_us(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000u+t.tv_nsec/1000u;
}

static struct voice_ns_probe *create_probe(void)
{
#ifdef __XTENSA__
  xtensa_coproc_enable(1<<3);
#endif
  struct voice_ns_probe *p=memalign(16,sizeof(*p));
  if(p)memset(p,0,sizeof(*p));
  if(!p)return NULL;
  p->noise=-1;
  p->frame_samples=NS_FRAME;
  if(g_probe_mode==3)
    {
      struct mallinfo mi=mallinfo();
      printf("[NNBudget] free=%u largest=%u required=1400000 min_largest=512000\n",(unsigned)mi.fordblks,(unsigned)mi.mxordblk);
      /* Measured397098B SDK payload +337790B assets, plus untracked
       * headers/cJSON/queues. This diagnostic headroom is not a guarantee
       * against arbitrary concurrent OOM; do not enable production NS yet. */
      if(mi.fordblks<1400000 || mi.mxordblk<512000)
        {printf("[NSProbe] create=blocked reason=NN-memory-budget\n");free(p);return NULL;}
      if(plant_nn_port_open()<0){free(p);return NULL;}
      char name[]="nsnet2";p->nn_api=plant_nn_esp_nsnet_handle_from_name(name);
      if(p->nn_api)p->nn=p->nn_api->create(name);
      if(!p->nn || p->nn_api->get_samp_chunksize(p->nn)!=512 || p->nn_api->get_samp_rate(p->nn)!=16000)
        {if(p->nn)p->nn_api->destroy(p->nn);plant_nn_port_close();free(p);return NULL;}
      p->frame_samples=512;
    }
  else p->handle=ns_pro_create(10,g_probe_mode,16000);
  if(!p->nn && !p->handle){free(p);return NULL;}
  p->vad=voice_vad_create();
  if(!p->vad)
    {if(p->nn){p->nn_api->destroy(p->nn);plant_nn_port_close();}else ns_destroy(p->handle);free(p);return NULL;}
  if(p->nn)plant_nn_pair_attach(p);
  return p;
}

struct voice_ns_probe *voice_ns_probe_create(void)
{
  if(!voice_ns_probe_requested())return NULL;
#ifdef __XTENSA__
  struct stackinfo_s info;volatile unsigned marker=0;
  if(nxsched_get_stackinfo(0,&info)<0 || info.adj_stack_size<15360 ||
     (uintptr_t)&marker<(uintptr_t)info.stack_base_ptr ||
     (uintptr_t)&marker-(uintptr_t)info.stack_base_ptr<8192)
    {printf("[NSProbe] create=blocked reason=worker-stack\n");return NULL;}
  printf("[NSProbe] runtime_stack=%u free_at_create=%u\n",
         (unsigned)info.adj_stack_size,
         (unsigned)((uintptr_t)&marker-(uintptr_t)info.stack_base_ptr));
#endif
  if(pthread_mutex_trylock(&g_probe_gate)!=0)return NULL;
  /* The SDK requests SPIRAM|8BIT for its 32948-byte state. Keep that
   * original policy: measured internal headroom cannot hold this state.
   * This is an explicitly enabled diagnostic; production PCM is untouched. */
  if(plant_ns_alloc_mode(false)<0)
    {pthread_mutex_unlock(&g_probe_gate);return NULL;}
  struct voice_ns_probe *p=create_probe();
  if(!p)pthread_mutex_unlock(&g_probe_gate);
  printf("[NSProbe] create=%s mode=%s frame_ms=%u production_pcm_unchanged=1\n",
         p?"ok":"failed",g_probe_mode==3?"neural":g_probe_mode==0?"mild":g_probe_mode==1?"medium":"aggressive",p?p->frame_samples/16:0);
  return p;
}

void voice_ns_probe_feed(struct voice_ns_probe *p,
                         const int16_t *samples,size_t count)
{
  if(!p || p->failed || (!samples&&count) || count>NS_INPUT_MAX)return;
  p->decision_valid=false;
  if(p->nn)plant_nn_pair_raw(p,samples,count);
  size_t produced=0;
  uint64_t start=now_us();
  while(count)
    {
      size_t take=p->frame_samples-p->used;if(take>count)take=count;
      memcpy(p->input+p->used,samples,take*sizeof(int16_t));
      p->used+=take;samples+=take;count-=take;
      if(p->used==p->frame_samples)
        {
          if(p->nn)
            {int rc=p->nn_api->process(p->nn,p->input,p->output);
             if(rc<0){p->failed=true;printf("[NSProbe] neural process_failed=%d production_pcm_unchanged=1\n",rc);return;}}
          else ns_process(p->handle,p->input,p->output);
          /* <= (count_at_entry + 511), bounded by NS_OUTPUT_MAX. */
          memcpy(p->clean+produced,p->output,p->frame_samples*sizeof(int16_t));
          produced+=p->frame_samples;p->used=0;p->frames++;
        }
    }
  uint32_t elapsed=(uint32_t)(now_us()-start);
  p->total_us+=elapsed;if(elapsed>p->max_us)p->max_us=elapsed;
  if(!produced)return;
  if(p->nn)plant_nn_pair_clean(p,p->clean,produced);
  int64_t sq=0;
  for(size_t i=0;i<produced;i++)sq+=(int64_t)p->clean[i]*p->clean[i];
  int32_t mean=(int32_t)(sq/produced);
  if(p->noise<0 || mean<p->noise)p->noise=mean;
  else p->noise+=(mean-p->noise)/64;
  bool speech=voice_vad_feed(p->vad,p->clean,produced);
  p->decision_valid=true;p->last_speech=speech;
  bool confirmed=voice_vad_started(p->vad);
  bool allowed=voice_onset_accept(&p->recent,mean,p->noise,speech,confirmed);
  if(p->nn)plant_nn_pair_mark(p,speech,allowed);
  p->onset_seen|=allowed;p->total+=produced;
  if(g_trace)printf("[NSProbe] total=%u output=%u mean=%ld noise=%ld vad=%d confirm=%d allowed=%d seen=%d us=%u\n",
         p->total,(unsigned)produced,(long)mean,(long)p->noise,
         speech,confirmed,allowed,p->onset_seen,elapsed);
}

void voice_ns_probe_destroy(struct voice_ns_probe *p)
{
  if(!p)return;
  printf("[NSProbe] end frames=%u residual=%u onset_seen=%d total_us=%llu max_block_us=%u\n",
         p->frames,(unsigned)p->used,p->onset_seen,(unsigned long long)p->total_us,p->max_us);
  if(g_endpoint_mode)printf("[Endpoint] done mode=%u blocks=%u selected_diff=%u raw_PCM=1 raw_onset=1\n",g_endpoint_mode,p->endpoint_blocks,p->endpoint_differences);
  bool neural=p->nn!=NULL;
  if(neural)plant_nn_pair_complete(p);
  voice_vad_destroy(p->vad);
  if(neural){p->nn_api->destroy(p->nn);plant_nn_port_close();}else ns_destroy(p->handle);
  free(p);
  if(!neural)plant_ns_alloc_report();
  pthread_mutex_unlock(&g_probe_gate);
}

static void *selftest_worker(void *unused)
{
  (void)unused;
  if(pthread_mutex_trylock(&g_probe_gate)!=0)return (void *)(intptr_t)-EBUSY;
#ifdef __XTENSA__
  xtensa_coproc_enable(1<<3);
#endif
  unsigned before=plant_ns_error_writes();
#ifdef __XTENSA__
  if(plant_ns_math_selftest()<0)
    {pthread_mutex_unlock(&g_probe_gate);return (void *)(intptr_t)-EIO;}
#endif
  /* Odd-sized real FFT must reject cleanly through the private error bridge. */
  void *invalid=plant_ns_esp_kiss_fftr_alloc(255,0,NULL,NULL);
  if(invalid || plant_ns_error_writes()!=before+1)
    {printf("[NSProbe] selftest failed: FFT error bridge\n");pthread_mutex_unlock(&g_probe_gate);return (void *)(intptr_t)-EIO;}
  bool all_ok=true;
  for(unsigned internal=0;internal<2;internal++)
  {
  if(plant_ns_alloc_mode(internal!=0)<0)
    {printf("[NSProbe] selftest failed: allocation preflight internal=%u\n",internal);all_ok=false;break;}
  struct voice_ns_probe *p=create_probe();
  if(!p){printf("[NSProbe] selftest failed: allocate internal=%u\n",internal);plant_ns_alloc_report();all_ok=false;break;}
  uint32_t random=1;uint64_t input_sq=0,output_sq=0,start=now_us();
  for(unsigned frame=0;frame<200;frame++)
    {
      for(unsigned i=0;i<NS_FRAME;i++)
        {random^=random<<13;random^=random>>17;random^=random<<5;p->input[i]=(int16_t)((int)(random%2001)-1000);}
      if(frame>=100)
        for(unsigned i=0;i<NS_FRAME;i++)
          input_sq+=(int64_t)p->input[i]*p->input[i];
      ns_process(p->handle,p->input,p->output);
      if(frame>=100)
        for(unsigned i=0;i<NS_FRAME;i++)output_sq+=(int64_t)p->output[i]*p->output[i];
    }
  uint64_t elapsed=now_us()-start;
  bool ok=input_sq>0 && output_sq<input_sq;
  printf("[NSProbe] selftest ok=%d frames=200 input_sq=%llu output_sq=%llu total_us=%llu mean_frame_us=%llu bridge_errors=1 alloc_internal=%u\n",
         ok,(unsigned long long)input_sq,(unsigned long long)output_sq,
         (unsigned long long)elapsed,(unsigned long long)(elapsed/200),internal);
  voice_vad_destroy(p->vad);ns_destroy(p->handle);free(p);
  if(plant_ns_alloc_report()<0)ok=false;
  all_ok&=ok;
  }
  plant_ns_alloc_mode(false);
  pthread_mutex_unlock(&g_probe_gate);
  return (void *)(intptr_t)(all_ok?0:-EIO);
}

int voice_ns_probe_selftest(void)
{
  if(g_probe_mode==3)return -EINVAL;
  pthread_t thread;pthread_attr_t attr;void *result;
  pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,16384);
  int rc=pthread_create(&thread,&attr,selftest_worker,NULL);pthread_attr_destroy(&attr);
  if(rc)return -rc;
  rc=pthread_join(thread,&result);
  return rc?-rc:(int)(intptr_t)result;
}

/* Synthetic smoke only. No microphone, speaker, network or production PCM path. */
#include "ns_nn_iface.h"
#include <malloc.h>
static void *nn_smoke_worker(void *unused)
{
  (void)unused;
  if(pthread_mutex_trylock(&g_probe_gate)!=0)return (void *)(intptr_t)-EBUSY;
#ifdef __XTENSA__
  xtensa_coproc_enable(1<<3);
#endif
  int result=-EIO;char name[]="nsnet2";
  const struct plant_nn_iface *api=NULL;plant_nn_data_t *model=NULL;
  int16_t *input=NULL,*output=NULL;
  struct mallinfo mi=mallinfo();
  if(mi.fordblks<1400000 || mi.mxordblk<512000)
    {printf("[NNProbe] blocked reason=NN-memory-budget\n");goto done;}
  if(plant_nn_port_open()<0){printf("[NNProbe] provider allocation failed\n");goto done;}
  api=plant_nn_esp_nsnet_handle_from_name(name);
  if(!api || !api->create || !api->process || !api->destroy)
    {printf("[NNProbe] interface unavailable\n");goto done;}
  printf("[NNProbe] create begin production_pcm_unchanged=1\n");
  uint64_t create_start=now_us();model=api->create(name);
  if(!model){printf("[NNProbe] model allocation failed\n");goto done;}
  int n=api->get_samp_chunksize(model),rate=api->get_samp_rate(model);
  printf("[NNProbe] create_ms=%u samples=%d rate=%d\n",(unsigned)((now_us()-create_start)/1000),n,rate);
  if(n<=0 || n>1024 || rate!=16000)goto done;
  input=memalign(16,2048+64);output=memalign(16,2048+64);
  if(!input || !output)goto done;
  uint32_t seed=1,max_us=0;uint64_t total_us=0,input_sq=0,output_sq=0;
  unsigned nonzero=0,frames=0,clipped=0;bool canary_ok=true,process_ok=true;
  for(unsigned frame=0;frame<200;frame++)
    {
      memset(input,0,2048+64);memset(output,0,2048+64);
      for(int i=0;i<n;i++)
        {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;input[i]=(int16_t)((int)(seed%2001u)-1000);}
      for(unsigned i=(unsigned)n;i<(unsigned)n+16;i++)output[i]=12345;
      uint64_t start=now_us();int rc=api->process(model,input,output);uint32_t elapsed=(uint32_t)(now_us()-start);
      total_us+=elapsed;if(elapsed>max_us)max_us=elapsed;frames++;
      if(rc<0){process_ok=false;break;}
      for(unsigned i=(unsigned)n;i<(unsigned)n+16;i++)if(output[i]!=12345)canary_ok=false;
      if(!canary_ok)break;
      if(frame>=100)
        for(int i=0;i<n;i++)
          {input_sq+=(int64_t)input[i]*input[i];output_sq+=(int64_t)output[i]*output[i];nonzero+=output[i]!=0;clipped+=output[i]>=32760||output[i]<=-32760;}
      if(total_us>12000000u){process_ok=false;break;}
    }
  /* This verifies execution/bounds only. Noise attenuation or nonzero output
   * alone is not speech quality, weak-child preservation or latency acceptance. */
  bool smoke_ok=process_ok && canary_ok && frames==200;
  printf("[NNProbe] smoke_ok=%d frames=%u samples=%d input_sq=%llu output_sq=%llu nonzero=%u clipped=%u total_us=%llu max_frame_us=%u bounds_ok=%d production_pcm_unchanged=1\n",
    smoke_ok,frames,n,(unsigned long long)input_sq,(unsigned long long)output_sq,nonzero,clipped,(unsigned long long)total_us,max_us,canary_ok);
  if(smoke_ok)result=0;
done:
  free(input);free(output);if(model && api)api->destroy(model);plant_nn_port_close();
  pthread_mutex_unlock(&g_probe_gate);return (void *)(intptr_t)result;
}
int voice_ns_nn_smoketest(void)
{
  pthread_t thread;pthread_attr_t attr;void *result;
  pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,16384);
  int rc=pthread_create(&thread,&attr,nn_smoke_worker,NULL);pthread_attr_destroy(&attr);
  if(rc)return -rc;rc=pthread_join(thread,&result);return rc?-rc:(int)(intptr_t)result;
}
