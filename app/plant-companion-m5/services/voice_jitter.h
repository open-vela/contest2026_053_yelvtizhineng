#ifndef PLANT_VOICE_JITTER_H
#define PLANT_VOICE_JITTER_H
/* A bounded HTTP producer and one audio consumer. Never hold the queue mutex
 * across a callback, socket read or join. Bytes/order/stream limit unchanged. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#define VOICE_JITTER_CAPACITY 65536u
#define VOICE_JITTER_RESERVE 24044u /* 500ms mono PCM plus WAV header */
struct voice_jitter_s
{
  pthread_mutex_t lock;
  pthread_cond_t changed;
  uint8_t *data;
  size_t head,used,high;
  bool done,cancelled;
  int result;
  const char *path;
  int stall;
  size_t maximum;
};
static int voice_jitter_put(const uint8_t *data,size_t length,void *arg)
{
  struct voice_jitter_s *q=arg;
  while(length)
    {
      pthread_mutex_lock(&q->lock);
      while(q->used==VOICE_JITTER_CAPACITY && !q->cancelled)
        pthread_cond_wait(&q->changed,&q->lock);
      if(q->cancelled){pthread_mutex_unlock(&q->lock);return -ECANCELED;}
      size_t tail=(q->head+q->used)%VOICE_JITTER_CAPACITY;
      size_t n=VOICE_JITTER_CAPACITY-q->used;
      if(n>VOICE_JITTER_CAPACITY-tail)n=VOICE_JITTER_CAPACITY-tail;
      if(n>length)n=length;
      memcpy(q->data+tail,data,n);q->used+=n;
      if(q->used>q->high)q->high=q->used;
      pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);
      data+=n;length-=n;
    }
  return 0;
}
static void *voice_jitter_receive(void *arg)
{
  struct voice_jitter_s *q=arg;
  int result=server_bridge_get_stream(q->path,voice_jitter_put,q,q->stall,q->maximum);
  pthread_mutex_lock(&q->lock);q->result=result;q->done=true;
  pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);
  return NULL;
}
static int voice_buffered_get_stream(const char *path,
                                    server_bridge_stream_cb_t callback,
                                    void *arg,int stall,size_t maximum)
{
  struct voice_jitter_s q={0};pthread_t thread;pthread_attr_t attr;
  uint8_t block[2048];int rc;bool primed=false;
  q.data=malloc(VOICE_JITTER_CAPACITY);if(!q.data)return -ENOMEM;
  q.path=path;q.stall=stall;q.maximum=maximum;
  rc=pthread_mutex_init(&q.lock,NULL);if(rc){free(q.data);return -rc;}
  rc=pthread_cond_init(&q.changed,NULL);
  if(rc){pthread_mutex_destroy(&q.lock);free(q.data);return -rc;}
  rc=pthread_attr_init(&attr);
  if(rc){rc=-rc;goto out;}
#ifdef VOICE_JITTER_HOST_TEST
  rc=pthread_attr_setstacksize(&attr,65536);
#else
  rc=pthread_attr_setstacksize(&attr,8192);
#endif
#ifndef VOICE_JITTER_HOST_TEST
  if(!rc)rc=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
  if(!rc)rc=pthread_attr_setschedpolicy(&attr,SCHED_RR);
  struct sched_param priority={.sched_priority=110};
  if(!rc)rc=pthread_attr_setschedparam(&attr,&priority);
#endif
  if(!rc)rc=pthread_create(&thread,&attr,voice_jitter_receive,&q);
  pthread_attr_destroy(&attr);
  if(rc){rc=-rc;goto out;}
  rc=0;
  for(;;)
    {
      if(ai_voice_stream_cancel_requested()){rc=-ECANCELED;break;}
      pthread_mutex_lock(&q.lock);
      /* The first block waits for a useful reserve, or a short complete WAV.
       * Subsequent blocks drain promptly, independently of recv() stalls. */
      while((!q.used || (!primed && q.used<VOICE_JITTER_RESERVE)) && !q.done)
        {
          struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
          until.tv_nsec+=100000000L;
          if(until.tv_nsec>=1000000000L){until.tv_sec++;until.tv_nsec-=1000000000L;}
          pthread_cond_timedwait(&q.changed,&q.lock,&until);
          if(ai_voice_stream_cancel_requested())break;
        }
      if(ai_voice_stream_cancel_requested())
        {rc=-ECANCELED;pthread_mutex_unlock(&q.lock);break;}
      if(!q.used && q.done){rc=q.result;pthread_mutex_unlock(&q.lock);break;}
      primed=true;
      size_t n=q.used;if(n>sizeof(block))n=sizeof(block);
      if(n>VOICE_JITTER_CAPACITY-q.head)n=VOICE_JITTER_CAPACITY-q.head;
      memcpy(block,q.data+q.head,n);q.head=(q.head+n)%VOICE_JITTER_CAPACITY;q.used-=n;
      pthread_cond_broadcast(&q.changed);pthread_mutex_unlock(&q.lock);
      rc=callback(block,n,arg);if(rc<0)break;
    }
  pthread_mutex_lock(&q.lock);q.cancelled=true;
  pthread_cond_broadcast(&q.changed);pthread_mutex_unlock(&q.lock);
  pthread_join(thread,NULL);
  printf("[VoiceBuffer] capacity=%u reserve=%u high=%u rc=%d\n",
         VOICE_JITTER_CAPACITY,VOICE_JITTER_RESERVE,(unsigned)q.high,rc);
out:
  pthread_cond_destroy(&q.changed);pthread_mutex_destroy(&q.lock);free(q.data);
  return rc>0?rc:(rc<0?rc:0);
}
#endif
