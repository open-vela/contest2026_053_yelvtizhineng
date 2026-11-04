#include "voice_upload.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

struct voice_upload
{
  const uint8_t *data;
  size_t capacity,sent;
  _Atomic size_t published;
  _Atomic bool finished,discard;
  pthread_t thread;
  voice_upload_begin_cb begin;
  voice_upload_send_cb send;
  voice_upload_cancel_cb cancelled;
  void *arg;
  int result;
};

static bool upload_cancelled(struct voice_upload *u)
{
  return atomic_load_explicit(&u->discard,memory_order_acquire) ||
         (u->cancelled && u->cancelled(u->arg));
}

static void *upload_worker(void *arg)
{
  struct voice_upload *u=arg;
  bool begun=false;
  for (;;)
    {
      if(upload_cancelled(u)){u->result=-ECANCELED;break;}
      bool finished=atomic_load_explicit(&u->finished,memory_order_acquire);
      size_t end=atomic_load_explicit(&u->published,memory_order_acquire);
      if(end>u->capacity || end<u->sent){u->result=-EOVERFLOW;break;}
      size_t available=end-u->sent;
      if(!available && finished)break;
      /* Start the session at the first confirmed speech/preroll callback.
       * Idle listening publishes no PCM, creates no server session. */
      if(available && !begun)
        {
          int rc=u->begin(u->arg);
          if(rc<0){u->result=rc;break;}
          begun=true;
          continue;
        }
      /* 256ms minimum amortizes HTTP round trips; at most one second per
       * request. A slow server simply leaves more immutable PCM queued. */
      if(available && (finished || available>=8192))
        {
          size_t take=available>32768?32768:available;
          if(upload_cancelled(u)){u->result=-ECANCELED;break;}
          int rc=u->send(u->data+u->sent,take,u->arg);
          if(rc<0){u->result=rc;break;}
          u->sent+=take;
          continue;
        }
      usleep(10000);
    }
  return NULL;
}

struct voice_upload *voice_upload_start(const uint8_t *buffer,size_t capacity,
    voice_upload_begin_cb begin,voice_upload_send_cb send,
    voice_upload_cancel_cb cancelled,void *arg)
{
  if(!buffer || !capacity || !begin || !send)return NULL;
  struct voice_upload *u=calloc(1,sizeof(*u));
  if(!u)return NULL;
  u->data=buffer;u->capacity=capacity;u->begin=begin;u->send=send;
  u->cancelled=cancelled;u->arg=arg;
  atomic_init(&u->published,0);atomic_init(&u->finished,false);
  atomic_init(&u->discard,false);
  pthread_attr_t attr;
  int rc=pthread_attr_init(&attr);
  if(rc){free(u);return NULL;}
  size_t stack_size=8192;
#ifdef PTHREAD_STACK_MIN
  if(stack_size<PTHREAD_STACK_MIN)stack_size=PTHREAD_STACK_MIN;
#endif
  rc=pthread_attr_setstacksize(&attr,stack_size);
  if(!rc)rc=pthread_create(&u->thread,&attr,upload_worker,u);
  pthread_attr_destroy(&attr);
  if(rc){free(u);return NULL;}
  return u;
}

void voice_upload_publish(struct voice_upload *u,size_t length)
{
  if(!u)return;
  /* A corrupt producer is a failed turn, never a truncated success. */
  if(length>u->capacity || length<atomic_load_explicit(&u->published,memory_order_relaxed))
    {atomic_store_explicit(&u->discard,true,memory_order_release);return;}
  atomic_store_explicit(&u->published,length,memory_order_release);
}

int voice_upload_finish(struct voice_upload *u,bool discard,size_t *sent)
{
  if(!u || !sent)return -EINVAL;
  *sent=SIZE_MAX;
  if(discard)atomic_store_explicit(&u->discard,true,memory_order_release);
  atomic_store_explicit(&u->finished,true,memory_order_release);
  int rc=pthread_join(u->thread,NULL);
  /* Join failure leaves ownership with the caller; never free memory still
   * reachable by the worker. A valid owner joins this private thread once. */
  if(rc)return -rc;
  *sent=u->sent;
  rc=u->result;
  free(u);
  return rc;
}
