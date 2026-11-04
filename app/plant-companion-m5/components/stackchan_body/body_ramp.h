/* SPDX-License-Identifier: MIT
 * Default spring equation derived from smooth_ui_toolkit v2.12.0 Spring,
 * Copyright (c)2023 Forairaaaaa. See LICENSE.smooth_ui_toolkit.
 * Official StackChan defaults k=170,c=26,m=1,v0=0 give decay13 and
 * damped frequency1. Fixed20ms animation steps match Servo::update.
 * Extra bounds: <=3raw ticks per20ms (<=150ticks/s), legal start/target,
 * no wall-clock catchup, cancellation, no commit on failed ACK.
 */
#ifndef PLANT_BODY_RAMP_H
#define PLANT_BODY_RAMP_H
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "motion_limits.h"
struct body_ramp {int current,target,id,start;uint32_t next_at,step;bool active,initial_anchor;};
static inline float body_spring_value(const struct body_ramp *r,float t,float *velocity)
{
 float delta=(float)(r->target-r->start),e=expf(-13.0f*t);
 *velocity=170.0f*delta*e*sinf(t);
 return (float)r->target-e*delta*(13.0f*sinf(t)+cosf(t));
}
static inline bool body_spring_rest(const struct body_ramp *r,float t)
{float velocity;float value=body_spring_value(r,t,&velocity);
 return fabsf(velocity)<=0.032f && fabsf((float)r->target-value)<=0.032f;}
static inline int body_ramp_begin(struct body_ramp *r,int id,int current,int target,uint32_t now)
{if(!r || !body_raw_valid(id,current) || !body_raw_valid(id,target))return -ERANGE;
 r->current=current;r->start=current;r->target=target;r->id=id;r->next_at=now;r->step=0;r->active=current!=target;r->initial_anchor=false;return 0;}
static inline bool body_ramp_next(const struct body_ramp *r,uint32_t now,int *target,unsigned *duration)
{if(!r->active || (int32_t)(now-r->next_at)<0)return false;
 if(r->initial_anchor){*target=r->current;*duration=20;return true;}
 float velocity,t=(float)(r->step+1)*0.02f;
 float v=body_spring_value(r,t,&velocity);
 int desired=body_spring_rest(r,t)?r->target:(int)floorf(v+0.5f);
 if(r->target>=r->start){if(desired<r->current)desired=r->current;if(desired>r->target)desired=r->target;}
 else {if(desired>r->current)desired=r->current;if(desired<r->target)desired=r->target;}
 int delta=desired-r->current;if(delta>3)delta=3;if(delta< -3)delta=-3;
 *target=r->current+delta;*duration=20;return true;}
static inline void body_ramp_commit(struct body_ramp *r,int target,unsigned duration,uint32_t sent_at)
{r->initial_anchor=false;r->current=target;r->next_at=sent_at+duration;r->step++;
 r->active=!(target==r->target && body_spring_rest(r,(float)r->step*0.02f));}
static inline void body_ramp_cancel(struct body_ramp *r){r->active=false;}
#endif
