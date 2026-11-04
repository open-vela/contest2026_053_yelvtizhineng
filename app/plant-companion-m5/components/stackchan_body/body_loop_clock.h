/* Match official Servo::update: period starts BEFORE work, not after it.
 * One absolute sleep per iteration; missed periods are never replayed.
 * Failure falls back to the previous conservative relative sleep.
 */
#ifndef BODY_LOOP_CLOCK_H
#define BODY_LOOP_CLOCK_H
#include <time.h>
#include <stdbool.h>
#include <unistd.h>
static unsigned g_body_absolute_sleeps,g_body_clock_fallbacks;
static inline bool body_loop_deadline(struct timespec *t)
{
 if(clock_gettime(CLOCK_MONOTONIC,t)<0)return false;
 t->tv_nsec+=20000000L;
 if(t->tv_nsec>=1000000000L){t->tv_sec++;t->tv_nsec-=1000000000L;}
 return true;
}
static inline void body_loop_wait(const struct timespec *t,bool valid)
{
 g_body_absolute_sleeps++;
 if(!valid || clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,t,0)!=0)
   {g_body_clock_fallbacks++;usleep(20000);}
}
#endif
