#pragma once
#include <stdbool.h>
#include <errno.h>

/* This unit's stationary trace has no replies at 260..730ms and stable
 * replies from 1010ms onward. Reserve 1200ms before the first operation.
 * These are initialization limits, not relaxed motion/packet deadlines. */
#define BODY_SERVO_SETTLE_MS 1200u
#define BODY_PREPARE_BUDGET_MS 2500u
#define BODY_PREPARE_WAIT_MS 3000u
enum body_power_state {BODY_OFF_IDLE=0,BODY_PREPARING=1,BODY_READY=2,BODY_FAULTED=3};
static inline bool body_power_may_prepare(int state){return state==BODY_OFF_IDLE;}
static inline int body_power_after_idle(int state){return state==BODY_FAULTED?BODY_FAULTED:BODY_OFF_IDLE;}
static inline int body_power_after_prepare(int state,int rc){
  if(state==BODY_FAULTED)return BODY_FAULTED;
  return rc==0?BODY_READY:rc==-ECANCELED?BODY_OFF_IDLE:BODY_FAULTED;
}
