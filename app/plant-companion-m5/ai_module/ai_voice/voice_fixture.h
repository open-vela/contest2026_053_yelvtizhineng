/* Explicit synthetic input only. Production defaults to inactive. */
#ifndef VOICE_FIXTURE_H
#define VOICE_FIXTURE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "voice_fixture_data.h"

struct fixture_cursor
{
  const int16_t *pcm;
  uint32_t count,pos,reference_end,reference_tick,program,turn,gen;
  bool active,just_ended,reference_pending,reference_valid;
};
static pthread_mutex_t fixture_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool fixture_active;
static uint32_t fixture_program,fixture_gen,fixture_next,fixture_turn;
static uint32_t fixture_born,fixture_reference_tick,fixture_pcm_streams;
static bool fixture_reference_valid;

static unsigned fixture_ms(uint32_t ticks)
{return (unsigned)((uint64_t)ticks*1000u/TICK_PER_SEC);}

static int fixture_arm(unsigned program,uint32_t gen,uint32_t now)
{
  if(program<1 || program>3)return -EINVAL;
  pthread_mutex_lock(&fixture_lock);
  int rc=atomic_load(&fixture_active)?-EBUSY:0;
  if(!rc)
    {
      fixture_program=program;fixture_gen=gen;fixture_next=0;fixture_turn=0;
      fixture_born=now;fixture_reference_valid=false;fixture_pcm_streams=0;
      atomic_store(&fixture_active,true);
    }
  pthread_mutex_unlock(&fixture_lock);return rc;
}
static void fixture_stop(void)
{
  if(!atomic_load(&fixture_active))return;
  pthread_mutex_lock(&fixture_lock);atomic_store(&fixture_active,false);
  pthread_mutex_unlock(&fixture_lock);
}
static bool fixture_expired(uint32_t now)
{
  if(!atomic_load(&fixture_active))return false;
  pthread_mutex_lock(&fixture_lock);
  bool expired=atomic_load(&fixture_active) && fixture_ms(now-fixture_born)>=180000;
  pthread_mutex_unlock(&fixture_lock);return expired;
}
static int fixture_take(uint32_t gen,struct fixture_cursor *cursor)
{
  *cursor=(struct fixture_cursor){0};
  pthread_mutex_lock(&fixture_lock);
  if(!atomic_load(&fixture_active) || fixture_gen!=gen)
    {pthread_mutex_unlock(&fixture_lock);return -ECANCELED;}
  unsigned clip=0;
  if(fixture_program==1 && fixture_next==0)clip=1;
  if(fixture_program==2 && fixture_next==0)clip=2;
  if(fixture_program==3 && fixture_next<3)
    {const unsigned clips[]={1,3,2};clip=clips[fixture_next];}
  fixture_next++;fixture_turn++;fixture_reference_valid=false;fixture_pcm_streams=0;
  cursor->active=true;cursor->program=fixture_program;cursor->turn=fixture_turn;cursor->gen=gen;
  if(clip==1){cursor->pcm=fixture_pcm_qa;cursor->count=sizeof(fixture_pcm_qa)/2;cursor->reference_end=FIXTURE_QA_REFERENCE_END;}
  if(clip==2){cursor->pcm=fixture_pcm_camera;cursor->count=sizeof(fixture_pcm_camera)/2;cursor->reference_end=FIXTURE_CAMERA_REFERENCE_END;}
  if(clip==3){cursor->pcm=fixture_pcm_novel;cursor->count=sizeof(fixture_pcm_novel)/2;cursor->reference_end=FIXTURE_NOVEL_REFERENCE_END;}
  pthread_mutex_unlock(&fixture_lock);
  printf("[Fixture] record program=%u turn=%u samples=%u source=synthetic after_end=zeros\n",cursor->program,cursor->turn,cursor->count);
  return 0;
}
static int16_t fixture_sample(struct fixture_cursor *cursor)
{
  uint32_t pos=cursor->pos;
  if(pos<UINT32_MAX)cursor->pos++;
  if(cursor->reference_end && cursor->pos==cursor->reference_end && pos<UINT32_MAX)
    cursor->just_ended=true;
  return pos<cursor->count?cursor->pcm[pos]:0;
}
static void fixture_reference(struct fixture_cursor *cursor,uint32_t now)
{
  cursor->just_ended=false;cursor->reference_tick=now;
  cursor->reference_pending=true;cursor->reference_valid=true;
  pthread_mutex_lock(&fixture_lock);
  if(atomic_load(&fixture_active) && fixture_gen==cursor->gen && fixture_turn==cursor->turn)
    {fixture_reference_tick=now;fixture_reference_valid=true;}
  pthread_mutex_unlock(&fixture_lock);
}
static void fixture_reference_emit(struct fixture_cursor *cursor)
{
  if(!cursor->reference_pending)return;
  cursor->reference_pending=false;
  printf("[Fixture] reference_end program=%u turn=%u tick=%u sample=%u\n",cursor->program,cursor->turn,cursor->reference_tick,cursor->reference_end);
}
static void fixture_capture_end(struct fixture_cursor *cursor,uint32_t now)
{
  if(!cursor->active)return;
  fixture_reference_emit(cursor);
  printf("[Fixture] capture_end program=%u turn=%u consumed=%u has_reference=%d reference_to_end_ms=%u\n",cursor->program,cursor->turn,cursor->pos,cursor->reference_valid,
         cursor->reference_valid?fixture_ms(now-cursor->reference_tick):0);
}
static void fixture_event(unsigned kind,uint32_t now)
{
  if(!atomic_load(&fixture_active))return;
  pthread_mutex_lock(&fixture_lock);
  if(atomic_load(&fixture_active) && fixture_reference_valid)
    {
      if(kind==2)fixture_pcm_streams++;
      printf("[FixturePerf] program=%u turn=%u kind=%u stream=%u reference_to_ms=%u tick=%u\n",fixture_program,fixture_turn,kind,fixture_pcm_streams,fixture_ms(now-fixture_reference_tick),now);
    }
  pthread_mutex_unlock(&fixture_lock);
}
static void fixture_status(void)
{
  pthread_mutex_lock(&fixture_lock);
  printf("[Fixture] active=%d program=%u records=%u reference=%d\n",atomic_load(&fixture_active),fixture_program,fixture_turn,fixture_reference_valid);
  pthread_mutex_unlock(&fixture_lock);
}
#endif
