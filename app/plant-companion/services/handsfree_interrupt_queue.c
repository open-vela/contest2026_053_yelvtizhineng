#include "handsfree_interrupt_queue.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <nuttx/clock.h>
#include "voice_service.h"
#include "handsfree_interrupt.h"

enum {EMPTY,PENDING,INFLIGHT};
struct interrupt_slot {int state;uint32_t token;size_t samples;char spoken[1024];int16_t pcm[48000];};
static pthread_mutex_t interrupt_lock=PTHREAD_MUTEX_INITIALIZER;
static struct interrupt_slot *slots[2];
static uint32_t active_token,result_token;
static pthread_cond_t pending_changed=PTHREAD_COND_INITIALIZER;
static bool worker_started;
static unsigned offered,processed,confirmed,unavailable;
static int result_hit;
void hf_interrupt_set_turn(uint32_t token)
{
  pthread_mutex_lock(&interrupt_lock);
  if(token!=active_token){
    active_token=token;result_token=0;
    for(int i=0;i<2;i++)if(slots[i]&&slots[i]->state!=INFLIGHT){free(slots[i]);slots[i]=NULL;}
  }
  pthread_mutex_unlock(&interrupt_lock);
}
int hf_interrupt_offer(uint32_t token,const int16_t *pcm,size_t samples,const char *spoken)
{
  if(!token||!pcm||samples<1600||samples>48000)return -EINVAL;
  pthread_mutex_lock(&interrupt_lock);
  if(token!=active_token){pthread_mutex_unlock(&interrupt_lock);return -ECANCELED;}
  int index=-1;
  /* Replace stale waiting audio rather than accumulating a network backlog. */
  for(int i=0;i<2;i++)if(slots[i]&&slots[i]->state==PENDING){index=i;break;}
  if(index<0)for(int i=0;i<2;i++)if(!slots[i]||slots[i]->state==EMPTY){index=i;break;}
  if(index<0){pthread_mutex_unlock(&interrupt_lock);return -EBUSY;}
  if(!slots[index])slots[index]=calloc(1,sizeof(*slots[index]));
  struct interrupt_slot *slot=slots[index];
  if(!slot){unavailable++;pthread_mutex_unlock(&interrupt_lock);return -ENOMEM;}
  memcpy(slot->pcm,pcm,samples*2);snprintf(slot->spoken,sizeof(slot->spoken),"%s",spoken?spoken:"");
  slot->samples=samples;slot->token=token;slot->state=PENDING;offered++;
  pthread_cond_signal(&pending_changed);
  pthread_mutex_unlock(&interrupt_lock);return 0;
}
int hf_interrupt_take(uint32_t token)
{
  pthread_mutex_lock(&interrupt_lock);
  int hit=token&&token==active_token&&result_token==token?result_hit:0;
  if(hit)result_token=0;
  pthread_mutex_unlock(&interrupt_lock);return hit;
}
void hf_interrupt_process(void)
{
  pthread_mutex_lock(&interrupt_lock);
  struct interrupt_slot *slot=NULL;
  for(int i=0;i<2;i++)if(slots[i]&&slots[i]->state==PENDING&&slots[i]->token==active_token){slot=slots[i];break;}
  if(!slot){pthread_mutex_unlock(&interrupt_lock);return;}
  slot->state=INFLIGHT;uint32_t token=slot->token;
  pthread_mutex_unlock(&interrupt_lock);
    int hit=0;
  if(voice_service_speaking_token()==token)
    hit=hf_interrupt_decode(true,true,true,slot->pcm,slot->samples,slot->spoken);
    uint32_t current=voice_service_speaking_token();
  pthread_mutex_lock(&interrupt_lock);
  processed++;
  if(hit&&token==active_token&&current==token){result_token=token;result_hit=hit;confirmed++;}
  slot->state=EMPTY;
  if(token!=active_token){for(int i=0;i<2;i++)if(slots[i]==slot){slots[i]=NULL;free(slot);break;}}
  pthread_mutex_unlock(&interrupt_lock);
}
/* Recognition must not wait behind telemetry/time HTTP requests. This
 * worker owns only the bounded interrupt queue and never dispatches UI. */
static void *interrupt_worker(void *arg)
{
 (void)arg;
 for(;;){
  pthread_mutex_lock(&interrupt_lock);
  for(;;){
   bool pending=false;
   for(int i=0;i<2;i++)if(slots[i]&&slots[i]->state==PENDING&&slots[i]->token==active_token)pending=true;
   if(pending)break;
   pthread_cond_wait(&pending_changed,&interrupt_lock);
  }
  pthread_mutex_unlock(&interrupt_lock);hf_interrupt_process();
 }
 return NULL;
}
int hf_interrupt_start(void)
{
 pthread_mutex_lock(&interrupt_lock);
 if(worker_started){pthread_mutex_unlock(&interrupt_lock);return 0;}
 pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,12288);
 pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);pthread_t thread;
 int ret=pthread_create(&thread,&attr,interrupt_worker,NULL);pthread_attr_destroy(&attr);
 if(!ret)worker_started=true;
 pthread_mutex_unlock(&interrupt_lock);return ret?-ret:0;
}
void hf_interrupt_queue_status(void)
{
  pthread_mutex_lock(&interrupt_lock);
  printf("[HFInterrupt] offered=%u processed=%u confirmed=%u allocation_failures=%u active=%lu\n",offered,processed,confirmed,unavailable,(unsigned long)active_token);
  pthread_mutex_unlock(&interrupt_lock);
}
