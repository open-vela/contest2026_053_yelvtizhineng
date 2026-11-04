/* Optional RAM-only diagnostic, disabled without an allocation. No PCM output.
 * Only the wake worker invokes the model. Capturing suppresses UI dispatch;
 * these detections are diagnostic and never a successful user wake/round.
 */
#define WAKE_CAPTURE_FRAMES 160u
#define WAKE_CAPTURE_CHUNK 512u
struct wake_capture
{
  int16_t pcm[WAKE_CAPTURE_FRAMES][WAKE_CAPTURE_CHUNK];
  unsigned mean[WAKE_CAPTURE_FRAMES],trigger[WAKE_CAPTURE_FRAMES];
  unsigned hold[WAKE_CAPTURE_FRAMES];
  unsigned frames,total,detected,maximum_probability;
  bool ready;
};
static pthread_mutex_t g_wake_capture_lock=PTHREAD_MUTEX_INITIALIZER;
static struct wake_capture *g_wake_capture;
static _Atomic bool g_wake_capture_present,g_wake_capture_reset;
static _Atomic unsigned g_wake_capture_replay;

static bool wake_capture_suppressed(void)
{
  return g_wake_capture_present || g_wake_capture_reset || g_wake_capture_replay;
}
static void wake_capture_frame(const int16_t *pcm,unsigned chunk,
                               unsigned mean,unsigned trigger,unsigned hold)
{
  if(!g_wake_capture_present)return;
  pthread_mutex_lock(&g_wake_capture_lock);
  struct wake_capture *c=g_wake_capture;
  if(c && !c->ready && chunk==WAKE_CAPTURE_CHUNK)
    {
      unsigned i=c->frames;
      if(i<WAKE_CAPTURE_FRAMES)
        {
          memcpy(c->pcm[i],pcm,chunk*sizeof(int16_t));
          c->mean[i]=mean;c->trigger[i]=trigger;c->hold[i]=hold;
          c->frames++;
          if(c->frames==WAKE_CAPTURE_FRAMES)
            {c->ready=true;printf("[WakeCapture] ready=1 frames=%u detected=%u max_probability=%u pcm_exported=0 dispatch=0\n",c->frames,c->detected,c->maximum_probability);}
        }
      c->total++;
    }
  pthread_mutex_unlock(&g_wake_capture_lock);
}
static void wake_capture_detected(unsigned probability)
{
  pthread_mutex_lock(&g_wake_capture_lock);
  struct wake_capture *c=g_wake_capture;
  if(c && !c->ready)
    {c->detected++;if(probability>c->maximum_probability)c->maximum_probability=probability;}
  pthread_mutex_unlock(&g_wake_capture_lock);
  printf("[WakeCapture] detection probability=%u/1000 dispatch=0 pcm_exported=0\n",probability);
}
/* Same linked SDK, same high-pass PCM, same gate times and score. The only
 * counterfactual is how much preceding real PCM is fed at an open gate. */
static void wake_capture_run_replay(unsigned preroll)
{
  unsigned accepted=0,maximum=0,first=WAKE_CAPTURE_FRAMES,missing=0,gates=0;
  pthread_mutex_lock(&g_wake_capture_lock);
  struct wake_capture *c=g_wake_capture;
  if(!c || !c->ready){pthread_mutex_unlock(&g_wake_capture_lock);return;}
  g_mn->clean(g_model);
  for(unsigned i=0;i<c->frames;i++)
    {
      bool gate=!c->hold[i] && c->mean[i]>c->trigger[i];
      unsigned count=gate?preroll+1:1;
      if(gate)
        {
          gates++;g_mn->clean(g_model);
          if(i<preroll){missing++;count=i+1;}
        }
      if(!gate && !c->hold[i])continue;
      for(unsigned k=0;k<count;k++)
        {
          unsigned index=i-(count-1)+k;
          esp_mn_state_t state=infer_frame(c->pcm[index]);
          if(state==ESP_MN_STATE_DETECTED)
            {
              esp_mn_results_t *r=g_mn->get_results(g_model);
              if(r && r->num>0 && r->command_id[0]==1)
                {
                  unsigned p=(unsigned)(r->prob[0]*1000);
                  if(p>maximum)maximum=p;
                  if(wake_match_accept(r,g_threshold/100.0f,NULL,NULL))
                    {accepted++;if(first==WAKE_CAPTURE_FRAMES)first=index;}
                }
              g_mn->clean(g_model);
            }
          else if(state==ESP_MN_STATE_TIMEOUT)g_mn->clean(g_model);
        }
      unsigned hold=gate?WAKE_DECODER_TAIL_FRAMES:c->hold[i];
      if(c->mean[i]>c->trigger[i]/2)hold=WAKE_DECODER_TAIL_FRAMES;
      else if(hold)hold--;
      if(!hold)g_mn->clean(g_model);
    }
  g_mn->clean(g_model);
  printf("[WakeReplay] preroll=%u frames=%u gates=%u context_missing=%u accepted=%u max_probability=%u first_frame=%u pcm_exported=0 dispatch=0 production_changed=0\n",preroll,c->frames,gates,missing,accepted,maximum,first);
  pthread_mutex_unlock(&g_wake_capture_lock);
}
static int wake_capture_command(int argc,char **argv)
{
  int rc=0;
  pthread_mutex_lock(&g_wake_capture_lock);
  if(argc==0 || (argc==1 && !strcmp(argv[0],"status")))
    {
      struct wake_capture *c=g_wake_capture;
      printf("[WakeCapture] allocated=%d ready=%d frames=%u total=%u detected=%u max_probability=%u replay_pending=%u bytes=%u pcm_exported=0 dispatch=%d\n",c!=NULL,c&&c->ready,c?c->frames:0,c?c->total:0,c?c->detected:0,c?c->maximum_probability:0,g_wake_capture_replay,c?(unsigned)sizeof(*c):0,c?0:1);
    }
  else if(argc==1 && !strcmp(argv[0],"off"))
    {
      g_wake_capture_reset=true;g_ready=false;g_wake_capture_present=false;
      g_wake_capture_replay=0;
      if(g_wake_capture)
        {
          volatile unsigned char *p=(volatile unsigned char *)g_wake_capture;
          for(size_t n=0;n<sizeof(*g_wake_capture);n++)p[n]=0;
          free(g_wake_capture);g_wake_capture=NULL;
        }
      printf("[WakeCapture] erased=1 allocated=0 pcm_exported=0\n");
    }
  else if(argc==1 && !strcmp(argv[0],"arm"))
    {
      if(voice_service_busy() || g_pause || !g_enabled || !g_model ||
         g_event || g_local_applied || g_wake_capture_replay)rc=-EBUSY;
      else
        {
          if(!g_wake_capture)g_wake_capture=calloc(1,sizeof(*g_wake_capture));
          else memset(g_wake_capture,0,sizeof(*g_wake_capture));
          if(!g_wake_capture)rc=-ENOMEM;
          else
            {
              g_wake_capture_reset=true;g_ready=false;g_wake_capture_present=true;
              printf("[WakeCapture] armed=1 frames_cap=%u bytes=%u pcm_exported=0 dispatch=0\n",WAKE_CAPTURE_FRAMES,(unsigned)sizeof(*g_wake_capture));
            }
        }
    }
  else if(argc==2 && !strcmp(argv[0],"replay"))
    {
      unsigned n=0;const char *p=argv[1];
      if(!*p)rc=-EINVAL;
      for(;*p && !rc;p++)
        {if(*p<'0'||*p>'9')rc=-EINVAL;else {n=n*10u+(*p-'0');if(n>48)rc=-EINVAL;}}
      if(!rc && (n<8 || !g_wake_capture || !g_wake_capture->ready))rc=-EINVAL;
      if(!rc && (voice_service_busy() || g_pause || g_wake_capture_replay))rc=-EBUSY;
      if(!rc){g_ready=false;g_wake_capture_replay=n;printf("[WakeCapture] replay_requested=%u pcm_exported=0 dispatch=0\n",n);}
    }
  else if(argc==1 && !strcmp(argv[0],"trace"))
    {
      if(!g_wake_capture || !g_wake_capture->ready)rc=-EBUSY;
      else for(unsigned i=0;i<g_wake_capture->frames;i++)
        printf("[WakeCaptureFrame] i=%u mean=%u trigger=%u hold=%u\n",i,g_wake_capture->mean[i],g_wake_capture->trigger[i],g_wake_capture->hold[i]);
      if(!rc)printf("[WakeCapture] trace_done=1 pcm_exported=0\n");
    }
  else rc=-EINVAL;
  pthread_mutex_unlock(&g_wake_capture_lock);return rc;
}
