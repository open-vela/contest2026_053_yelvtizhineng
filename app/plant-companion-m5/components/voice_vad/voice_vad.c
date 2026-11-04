/* Official ESP-SR WebRTC VAD, mode 0 as in the M5 XiaoZhi AFE path.
 * Keep 30 ms frames continuous across the recorder's 125.25 ms blocks.
 * This is VAD only, not an implementation of the full AFE/AEC pipeline.
 */
#include "voice_vad.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include "../voice_ns/ns_nn_pair.h"
/* Retain the 12-block child pause window. Mode0 alone intermittently marks
 * low-energy room noise as speech and prevents that window from completing.
 * Corroborate only the tail with the existing independent mode3 model;
 * onset, raw PCM, pre-roll and energy guards remain unchanged. */
static _Atomic unsigned g_gmm_endpoint = 3;
static _Atomic unsigned g_gmm_live;
#define vad_create plant_vad_vad_create
#define vad_process plant_vad_vad_process
#define vad_destroy plant_vad_vad_destroy
#include "vendor/esp_vad.h"

struct voice_vad
{
  vad_handle_t handle,endpoint,onset;
  unsigned endpoint_mode,blocks,different;
  bool endpoint_speech,onset_speech;
  int16_t frame[480];
  size_t used;
  unsigned consecutive;
  unsigned strict_run,strict_frames,strict_total;
  bool strict_confirmed;
  unsigned warmup;
  bool started;
};

static void capture_attach(struct voice_vad *v);
static void capture_frame(struct voice_vad *v,const int16_t *pcm,bool speech);
static void capture_detach(struct voice_vad *v);

struct voice_vad *voice_vad_create(void)
{
  struct voice_vad *v=calloc(1,sizeof(*v));
  if(v)
    {
      v->handle=vad_create(VAD_MODE_0);
      if(!v->handle){free(v);return NULL;}
      /* The statistical model initially labels quiet microphone noise as
       * speech for a short hangover. Feed 180ms to adapt before confirming
       * onset. The recorder retains all of these samples in its pre-roll. */
      v->warmup=6;
      v->endpoint_mode=g_gmm_endpoint;
      if(v->endpoint_mode)
        {
          v->endpoint=vad_create(v->endpoint_mode==3?VAD_MODE_3:VAD_MODE_2);
          if(!v->endpoint){v->endpoint_mode=0;printf("[GMMEnd] fallback=raw allocation_failed=1\n");}
          else plant_nn_pair_attach(v);
        }
      /* Mode 0 remains the continuous recorder/tail decision. A separate
       * mode 3 vote must corroborate onset, including when endpoint testing
       * is disabled. Reuse that model when endpoint mode 3 already owns it. */
      if(!v->endpoint || v->endpoint_mode!=3)
        {
          v->onset=vad_create(VAD_MODE_3);
          if(!v->onset)
            {
              if(v->endpoint){plant_nn_pair_complete(v);vad_destroy(v->endpoint);}
              vad_destroy(v->handle);free(v);
              printf("[VAD] strict_onset_allocation_failed=1\n");return NULL;
            }
        }
      atomic_fetch_add(&g_gmm_live,1);
      capture_attach(v);
    }
  return v;
}

void voice_vad_destroy(struct voice_vad *v)
{
  if(v){capture_detach(v);
    if(v->endpoint){plant_nn_pair_complete(v);vad_destroy(v->endpoint);
      printf("[GMMEnd] done mode=%u blocks=%u differences=%u raw_PCM=1 raw_onset=0 strict_onset=1\n",v->endpoint_mode,v->blocks,v->different);}
    if(v->onset)vad_destroy(v->onset);
    vad_destroy(v->handle);atomic_fetch_sub(&g_gmm_live,1);free(v);}
}

bool voice_vad_feed(struct voice_vad *v,const int16_t *samples,size_t count)
{
  bool voiced=false;
  if(!v || (!samples && count))return false;
  /* Confirmation belongs to this feed, not an earlier rejected noise burst.
   * Consecutive frames still carry across feeds. Once the recorder accepts
   * onset it no longer consults this flag.
   */
  if(count){v->started=false;v->endpoint_speech=false;v->onset_speech=false;v->strict_frames=0;v->strict_total=0;v->strict_confirmed=false;}
  if(v->endpoint)plant_nn_pair_raw(v,samples,count);
  while(count)
    {
      size_t take=480-v->used;if(take>count)take=count;
      memcpy(v->frame+v->used,samples,take*sizeof(*samples));
      v->used+=take;samples+=take;count-=take;
      if(v->used==480)
        {
          bool speech=vad_process(v->handle,v->frame,16000,30)==VAD_SPEECH;
          bool endpoint=v->endpoint?vad_process(v->endpoint,v->frame,16000,30)==VAD_SPEECH:speech;
          bool strict=v->onset?vad_process(v->onset,v->frame,16000,30)==VAD_SPEECH:endpoint;
          capture_frame(v,v->frame,speech);
          if(v->warmup){v->warmup--;speech=false;endpoint=false;strict=false;}
          v->endpoint_speech|=endpoint;
          v->onset_speech|=strict;
          v->strict_total++;v->strict_frames+=strict;
          v->strict_run=strict?(v->strict_run<4?v->strict_run+1:4):0;
          if(v->strict_run>=4)v->strict_confirmed=true;
          v->consecutive=speech?v->consecutive+1:0;
          if(v->consecutive>=4)v->started=true; /* 120 ms confirmed speech */
          voiced|=speech;v->used=0;
        }
    }
  return voiced;
}

bool voice_vad_started(const struct voice_vad *v){return v && v->started;}
bool voice_vad_onset_corroborated(const struct voice_vad *v){return v && v->onset_speech;}

void voice_vad_strict_trace(const struct voice_vad *v,unsigned *frames,unsigned *total,unsigned *run,bool *confirmed)
{
 if(frames)*frames=v?v->strict_frames:0;if(total)*total=v?v->strict_total:0;
 if(run)*run=v?v->strict_run:0;if(confirmed)*confirmed=v && v->strict_confirmed;
}

unsigned voice_vad_endpoint_get_mode(void){return g_gmm_endpoint;}
int voice_vad_endpoint_set_mode(unsigned mode)
{
  if(mode>3)return -EINVAL;
  if(g_gmm_live)return -EBUSY;
  g_gmm_endpoint=mode;return 0;
}
bool voice_vad_endpoint_vote(struct voice_vad *v,bool waiting,bool raw,bool onset)
{
  if(!v || !v->endpoint)return raw;
  /* Endpoint can only shorten the maintained raw decision, never extend it.
   * Waiting retains mode 0 here; onset is independently corroborated by
   * mode 3 in the recorder. Uploaded PCM is unchanged. */
  bool candidate=raw && v->endpoint_speech;
  bool selected=waiting || v->endpoint_mode<2?raw:candidate;
  plant_nn_pair_raw_mark(v,waiting && onset,waiting?raw:candidate);
  v->blocks++;v->different+=selected!=raw;
  printf("[GMMEnd] mode=%u waiting=%d raw=%d candidate=%d selected=%d onset=%d\n",
         v->endpoint_mode,waiting,raw,candidate,selected,onset);
  return selected;
}


/* Optional bounded RAM-only diagnostics. This code NEVER changes VAD decisions
 * or sends PCM anywhere. Replay calls the linked SDK, not a host approximation.
 * arm N selects the Nth detector created after arming. off erases/releases RAM.
 * No allocations, PCM copies or frame mutexes while this feature is disabled.
 */
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#define CAPTURE_FRAMES 533u /* 15.99s, 511680 bytes of mono16k PCM */
struct vad_capture
{
  int16_t pcm[CAPTURE_FRAMES][480];
  unsigned char speech[CAPTURE_FRAMES];
  unsigned frames,total;
  void *owner;
  bool ready,keep;
  unsigned recorded_ms;
};
static pthread_mutex_t g_capture_lock=PTHREAD_MUTEX_INITIALIZER;
static struct vad_capture *g_capture;
static _Atomic(void *) g_capture_owner;
static unsigned g_capture_skip,g_capture_budget,g_capture_discarded;
static bool g_capture_auto;

static void capture_attach(struct voice_vad *v)
{
  pthread_mutex_lock(&g_capture_lock);
  if(g_capture && !g_capture->owner && !g_capture->ready && g_capture_skip && (!g_capture_auto || g_capture_budget))
    {
      if(--g_capture_skip==0)
        {g_capture->owner=v;if(g_capture_auto)g_capture_budget--;atomic_store(&g_capture_owner,v);}
    }
  pthread_mutex_unlock(&g_capture_lock);
}
static void capture_frame(struct voice_vad *v,const int16_t *pcm,bool speech)
{
  if(atomic_load(&g_capture_owner)!=v)return;
  pthread_mutex_lock(&g_capture_lock);
  if(g_capture && g_capture->owner==v && !g_capture->ready)
    {
      unsigned i=g_capture->frames;
      if(i<CAPTURE_FRAMES)
        {
          memcpy(g_capture->pcm[i],pcm,480*sizeof(int16_t));
          g_capture->speech[i]=speech;g_capture->frames++;
        }
      g_capture->total++;
    }
  pthread_mutex_unlock(&g_capture_lock);
}
static void capture_detach(struct voice_vad *v)
{
  if(atomic_load(&g_capture_owner)!=v)return;
  pthread_mutex_lock(&g_capture_lock);
  if(g_capture && g_capture->owner==v)
    {
      g_capture->owner=NULL;atomic_store(&g_capture_owner,NULL);
      if(g_capture_auto && !g_capture->keep)
        {g_capture->frames=0;g_capture->total=0;g_capture->recorded_ms=0;
         g_capture_discarded++;g_capture_skip=g_capture_budget?1:0;}
      else g_capture->ready=true;
    }
  pthread_mutex_unlock(&g_capture_lock);
}
void voice_vad_capture_complete(struct voice_vad *v,unsigned recorded_ms,bool triggered)
{
  if(!v || atomic_load(&g_capture_owner)!=v)return;
  pthread_mutex_lock(&g_capture_lock);
  if(g_capture && g_capture->owner==v)
    {g_capture->recorded_ms=recorded_ms;g_capture->keep=triggered && recorded_ms>=4000u;}
  pthread_mutex_unlock(&g_capture_lock);
}
static void capture_erase(void *memory,size_t bytes)
{
  volatile unsigned char *p=memory;
  while(bytes--)*p++=0;
}
static bool capture_number(const char *s,unsigned max,unsigned *value)
{
  if(!s || !*s)return false;
  unsigned n=0;
  for(;*s;s++)
    {if(*s<'0'||*s>'9')return false;n=n*10u+(unsigned)(*s-'0');if(n>max)return false;}
  *value=n;return true;
}
int voice_vad_capture_command(int argc,char **argv)
{
  int rc=0;
  pthread_mutex_lock(&g_capture_lock);
  if(argc==0 || (argc==1 && !strcmp(argv[0],"status")))
    {
      printf("[VADCapture] allocated=%d active=%d ready=%d skip=%u frames=%u total=%u truncated=%d cap_ms=15990 auto=%d budget=%u discarded=%u recorded_ms=%u pcm_exported=0\n",
             g_capture!=NULL,g_capture && g_capture->owner!=NULL,g_capture && g_capture->ready,
             g_capture_skip,g_capture?g_capture->frames:0,g_capture?g_capture->total:0,
             g_capture && g_capture->total>CAPTURE_FRAMES,g_capture_auto,g_capture_budget,g_capture_discarded,g_capture?g_capture->recorded_ms:0);
    }
  else if(argc==1 && !strcmp(argv[0],"off"))
    {
      if(g_capture && g_capture->owner)rc=-EBUSY;
      else
        {if(g_capture){capture_erase(g_capture,sizeof(*g_capture));free(g_capture);g_capture=NULL;}
         g_capture_skip=g_capture_budget=g_capture_discarded=0;g_capture_auto=false;printf("[VADCapture] erased=1 allocated=0\n");}
    }
  else if(argc==2 && !strcmp(argv[0],"arm"))
    {
      unsigned n=1;bool automatic=!strcmp(argv[1],"slow");
      if(!automatic && (!capture_number(argv[1],16,&n)||n==0))rc=-EINVAL;
      else if(g_capture)rc=-EBUSY;
      else
        {
          g_capture=calloc(1,sizeof(*g_capture));
          if(!g_capture)rc=-ENOMEM;
          else{g_capture_skip=n;g_capture_auto=automatic;g_capture_budget=automatic?16:0;g_capture_discarded=0;printf("[VADCapture] armed=%u bytes=%u pcm_exported=0\n",n,(unsigned)sizeof(*g_capture));}
        }
    }
  else if(argc>=1 && argc<=3 && !strcmp(argv[0],"replay"))
    {
      unsigned mode=0,start=0;
      if((argc>=2 && !capture_number(argv[1],4,&mode)) ||
         (argc>=3 && !capture_number(argv[2],CAPTURE_FRAMES,&start)))rc=-EINVAL;
      else if(!g_capture || !g_capture->ready || g_capture->owner)rc=-EBUSY;
      else if(start>=g_capture->frames)rc=-EINVAL;
      else
        {
          vad_handle_t handle=vad_create((vad_mode_t)mode);
          if(!handle)rc=-ENOMEM;
          else
            {
              unsigned differences=0,original=0,replayed=0,first=CAPTURE_FRAMES,last=0;
              unsigned run=0,longest=0;
              for(unsigned i=start;i<g_capture->frames;i++)
                {
                  bool speech=vad_process(handle,g_capture->pcm[i],16000,30)==VAD_SPEECH;
                  original+=g_capture->speech[i];replayed+=speech;
                  differences+=(speech!=g_capture->speech[i]);
                  if(speech){if(first==CAPTURE_FRAMES)first=i;last=i;run++;if(run>longest)longest=run;}else run=0;
                }
              vad_destroy(handle);
              printf("[VADReplay] mode=%u start=%u frames=%u original=%u replayed=%u differences=%u first=%u last=%u longest_ms=%u truncated=%d pcm_exported=0 production_changed=0\n",
                mode,start,g_capture->frames-start,original,replayed,differences,first,last,longest*30,
                g_capture->total>CAPTURE_FRAMES);
            }
        }
    }
  else rc=-EINVAL;
  pthread_mutex_unlock(&g_capture_lock);
  return rc;
}
