/* Private toolchain libm bridges, only referenced by wake SDK math imports. */
#include "wake_math.h"
#include <math.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

int *plant_wake_math_errno(void) { return &errno; }
float plant_wake_math_sqrtf(float x)
{
  if(x==0.0f || (isinf(x) && x>0.0f))return x;
  if(isnan(x))return x+x;
  if(x<0.0f){errno=EDOM;return NAN;}
  return sqrtf(x);
}

#ifdef __XTENSA__
#include "math_vectors.h"
int plant_wake_math_selftest(void)
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
  printf("[WakeMath] float=%u double=%u bad=%u max_float_ulp=%u max_double_ulp=%llu specials=%d\n",
         nfloat,ndouble,bad,max_float_ulp,(unsigned long long)max_double_ulp,special);
  return bad || !special?-EIO:0;
}
#else
int plant_wake_math_selftest(void) { return -ENOSYS; }
#endif
