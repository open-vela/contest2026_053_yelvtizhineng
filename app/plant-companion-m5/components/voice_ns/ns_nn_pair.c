/* Explicit paired diagnostic. Disabled by default; no audio in console logs.
 * Same microphone timeline, full512-sample frames only. No production VAD
 * decision changes. Only a locally configured private collector accepts upload.
 */
#include "ns_nn_pair.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>

#define PAIR_SAMPLES 128000u
struct nn_pair_capture {
 const void *owner;
 size_t raw_n,clean_n,raw_total,clean_total;
 bool ready;
 size_t previous[5],previous_pos,first_start,first_onset,first_end,later_speech;
 unsigned silent;
 size_t raw_start,raw_onset,candidate_end,later_candidate;unsigned candidate_silent;
 int16_t raw[PAIR_SAMPLES],clean[PAIR_SAMPLES];
};
static struct nn_pair_capture *g_pair;
static _Atomic(const void *) g_pair_owner;
static pthread_mutex_t g_pair_lock=PTHREAD_MUTEX_INITIALIZER;
extern int server_bridge_nn_pair_upload(const char *,const int16_t *,size_t);

void plant_nn_pair_attach(const void *owner)
{
 if(!owner)return;
 pthread_mutex_lock(&g_pair_lock);
 if(g_pair && !g_pair->ready && !g_pair->owner)
  {g_pair->owner=owner;atomic_store(&g_pair_owner,owner);printf("[NNPair] recording=1 max_samples=%u\n",PAIR_SAMPLES);}
 pthread_mutex_unlock(&g_pair_lock);
}
static void append(const void *owner,const int16_t *pcm,size_t n,bool clean)
{
 if(!owner || atomic_load(&g_pair_owner)!=owner || (!pcm&&n))return;
 pthread_mutex_lock(&g_pair_lock);
 if(g_pair && g_pair->owner==owner && !g_pair->ready)
  {
   size_t *used=clean?&g_pair->clean_n:&g_pair->raw_n;
   size_t *total=clean?&g_pair->clean_total:&g_pair->raw_total;
   int16_t *data=clean?g_pair->clean:g_pair->raw;
   size_t take=n;if(take>PAIR_SAMPLES-*used)take=PAIR_SAMPLES-*used;
   if(take)memcpy(data+*used,pcm,take*sizeof(int16_t));
   *used+=take;*total+=n;
   if(!clean){g_pair->previous[g_pair->previous_pos]=n;g_pair->previous_pos=(g_pair->previous_pos+1)%5;}
  }
 pthread_mutex_unlock(&g_pair_lock);
}
void plant_nn_pair_raw(const void *owner,const int16_t *pcm,size_t n){append(owner,pcm,n,false);}
void plant_nn_pair_clean(const void *owner,const int16_t *pcm,size_t n){append(owner,pcm,n,true);}
void plant_nn_pair_mark(const void *owner,int speech,int allowed)
{
 if(!owner || atomic_load(&g_pair_owner)!=owner)return;
 pthread_mutex_lock(&g_pair_lock);
 if(g_pair && g_pair->owner==owner && !g_pair->ready)
  {
   if(allowed && !g_pair->first_onset)
    {size_t pre=0;for(unsigned i=0;i<5;i++)pre+=g_pair->previous[i];
     g_pair->first_onset=g_pair->raw_total;g_pair->first_start=g_pair->raw_total-pre;}
   if(g_pair->first_onset)
    {g_pair->silent=speech?0:g_pair->silent+1;
     if(g_pair->first_end && speech)g_pair->later_speech++;
     if(g_pair->silent>=5 && !g_pair->first_end)g_pair->first_end=g_pair->raw_total;}
  }
 pthread_mutex_unlock(&g_pair_lock);
}
void plant_nn_pair_raw_mark(const void *owner,int onset,int selected)
{
 if(!owner || atomic_load(&g_pair_owner)!=owner)return;
 pthread_mutex_lock(&g_pair_lock);
 if(g_pair && g_pair->owner==owner && !g_pair->ready)
  {
   if(onset && !g_pair->raw_onset)
    {size_t pre=0;for(unsigned i=0;i<5;i++)pre+=g_pair->previous[i];
     g_pair->raw_onset=g_pair->raw_total;g_pair->raw_start=g_pair->raw_total-pre;}
   if(g_pair->raw_onset)
    {g_pair->candidate_silent=selected?0:g_pair->candidate_silent+1;
     if(g_pair->candidate_end && selected)g_pair->later_candidate++;
     if(g_pair->candidate_silent>=5 && !g_pair->candidate_end)g_pair->candidate_end=g_pair->raw_total;}
  }
 pthread_mutex_unlock(&g_pair_lock);
}
void plant_nn_pair_complete(const void *owner)
{
 if(!owner || atomic_load(&g_pair_owner)!=owner)return;
 pthread_mutex_lock(&g_pair_lock);
 if(g_pair && g_pair->owner==owner)
  {g_pair->owner=NULL;g_pair->ready=true;atomic_store(&g_pair_owner,NULL);
   printf("[NNPairRaw] start=%u onset=%u candidate_end=%u later_speech=%u\n",(unsigned)g_pair->raw_start,(unsigned)g_pair->raw_onset,(unsigned)g_pair->candidate_end,(unsigned)g_pair->later_candidate);
   printf("[NNPair] ready=1 raw=%u clean=%u truncated=%d start=%u onset=%u end=%u later_speech=%u\n",(unsigned)g_pair->raw_n,(unsigned)g_pair->clean_n,g_pair->raw_total>PAIR_SAMPLES||g_pair->clean_total>PAIR_SAMPLES,(unsigned)g_pair->first_start,(unsigned)g_pair->first_onset,(unsigned)g_pair->first_end,(unsigned)g_pair->later_speech);}
 pthread_mutex_unlock(&g_pair_lock);
}
static void erase(void *p,size_t n){volatile unsigned char *b=p;while(n--)*b++=0;}
int plant_nn_pair_command(int argc,char **argv)
{
 int rc=0;pthread_mutex_lock(&g_pair_lock);
 if(argc==0 || (argc==1 && !strcmp(argv[0],"status")))
  printf("[NNPair] allocated=%d active=%d ready=%d raw=%u clean=%u raw_total=%u clean_total=%u\n",g_pair!=NULL,g_pair && g_pair->owner!=NULL,g_pair && g_pair->ready,g_pair?(unsigned)g_pair->raw_n:0,g_pair?(unsigned)g_pair->clean_n:0,g_pair?(unsigned)g_pair->raw_total:0,g_pair?(unsigned)g_pair->clean_total:0);
 else if(argc==1 && !strcmp(argv[0],"arm"))
  {if(g_pair)rc=-EBUSY;else {g_pair=calloc(1,sizeof(*g_pair));if(!g_pair)rc=-ENOMEM;else printf("[NNPair] armed=1 bytes=%u cap_ms=8000\n",(unsigned)sizeof(*g_pair));}}
 else if(argc==1 && !strcmp(argv[0],"off"))
  {if(g_pair && g_pair->owner)rc=-EBUSY;else {if(g_pair){erase(g_pair,sizeof(*g_pair));free(g_pair);g_pair=NULL;}printf("[NNPair] erased=1 allocated=0\n");}}
 else if(argc==3 && !strcmp(argv[0],"upload"))
  {
   bool clean=!strcmp(argv[1],"clean");
   if(!clean && strcmp(argv[1],"raw"))rc=-EINVAL;
   else if(!g_pair || !g_pair->ready || g_pair->owner)rc=-EBUSY;
   else if(g_pair->raw_total>PAIR_SAMPLES || g_pair->clean_total>PAIR_SAMPLES)rc=-EMSGSIZE;
   else
    {
     size_t common=clean?g_pair->raw_n/512u*512u:g_pair->raw_n;
     if(!common || (clean && g_pair->clean_n!=common))rc=-EIO;
     else {rc=server_bridge_nn_pair_upload(argv[2],clean?g_pair->clean:g_pair->raw,common);
      printf("[NNPair] uploaded=%d samples=%u clean=%d rc=%d\n",rc==0,(unsigned)common,clean,rc);}
    }
  }
 else rc=-EINVAL;
 pthread_mutex_unlock(&g_pair_lock);return rc;
}
