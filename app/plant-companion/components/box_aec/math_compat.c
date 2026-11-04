/* Private ESP-SR math compatibility and inactive maintenance selftest. */
#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
/* Bridges required by the isolated toolchain math closure. */
int *plant_wake_math_errno(void){return &errno;}
float plant_wake_math_sqrtf(float x)
{
  if(x==0.0f || (isinf(x) && x>0.0f))return x;
  if(isnan(x))return x+x;
  if(x<0.0f){errno=EDOM;return NAN;}
  return sqrtf(x);
}

#ifndef BOX_AEC_HOST_TEST
#include "math_vectors.h"
#include <nuttx/clock.h>
int box_voice_math_selftest(void)
{
  unsigned bad=0,nfloat=0,ndouble=0,max_float_ulp=0;
  uint64_t max_double_ulp=0;
  for(unsigned i=0;i<sizeof(wake_math_cases)/sizeof(wake_math_cases[0]);i++)
    {
      const struct wake_math_case *c=&wake_math_cases[i];double got=wake_math_eval(c);
      if(!isfinite(got)){bad++;continue;}
      if(c->single)
        {
          float a=(float)got,b=(float)c->expected;uint32_t x,y;
          memcpy(&x,&a,4);memcpy(&y,&b,4);unsigned d=x>y?x-y:y-x;
          nfloat++;if(d>max_float_ulp)max_float_ulp=d;if(d>4)bad++;
        }
      else
        {
          uint64_t x,y;memcpy(&x,&got,8);memcpy(&y,&c->expected,8);
          uint64_t d=x>y?x-y:y-x;
          ndouble++;if(d>max_double_ulp)max_double_ulp=d;if(d>4)bad++;
        }
    }
  bool special=signbit(plant_wake_fast_sqrtf(-0.0f)) &&
               isinf(plant_wake_fast_sqrtf(INFINITY)) &&
               isnan(plant_wake_fast_sqrtf(-1.0f)) &&
               isnan(plant_wake_fast_logf(-1.0f)) &&
               isinf(plant_wake_fast_logf(0.0f));
  printf("[HFMath] float=%u double=%u bad=%u max_float_ulp=%u max_double_ulp=%llu specials=%d\n",
         nfloat,ndouble,bad,max_float_ulp,(unsigned long long)max_double_ulp,special);
  for(int fast=0;fast<2;fast++)
    {
      volatile float sink=0;clock_t began=clock_systime_ticks();
      for(int i=0;i<10000;i++)
        {
          float x=(i%99+1)*.1f;
          sink+=fast?plant_wake_fast_logf(x):logf(x);
          sink+=fast?plant_wake_fast_expf(x*.1f):expf(x*.1f);
          sink+=fast?plant_wake_fast_sinf(x):sinf(x);
        }
      printf("[HFMath] benchmark private=%d calls=30000 elapsed_ms=%lu finite=%d\n",fast,
        (unsigned long)((clock_systime_ticks()-began)*1000/TICK_PER_SEC),isfinite(sink));
    }
  return bad || !special?-EIO:0;
}

#endif
