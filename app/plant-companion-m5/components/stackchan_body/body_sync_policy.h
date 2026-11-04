/* Official SCS SyncWrite format, restricted to two known axes and SRAM
 * position/time/speed or torque-OFF only. A broadcast is not a completion:
 * the caller must read back every written goal, then fresh health feedback.
 */
#ifndef PLANT_BODY_SYNC_POLICY_H
#define PLANT_BODY_SYNC_POLICY_H
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "motion_limits.h"
struct body_sync_window {bool active;uint32_t started;};
static inline unsigned body_sync_budget(const struct body_sync_window *w,uint32_t now,unsigned limit)
{if(!w->active)return limit;uint32_t used=now-w->started;return used>=limit?0:limit-used;}
static inline bool body_sync_payload_valid(const uint8_t *p,int count)
{
 if(!p || count<4 || count>16)return false;
 unsigned reg=p[0],len=p[1];
 if(!((reg==40 && len==1)||(reg==42 && len==6)))return false;
 if((count-2)%(len+1))return false;
 unsigned axes=(count-2)/(len+1),seen=0;if(axes<1 || axes>2)return false;
 for(unsigned off=2;off<(unsigned)count;off+=len+1)
  {unsigned id=p[off];if(id<1 || id>2 || (seen&(1u<<(id-1))))return false;seen|=1u<<(id-1);
   if(reg==40){if(p[off+1]!=0)return false;}
   else
    {int target=((unsigned)p[off+1]<<8)|p[off+2];
     unsigned time=((unsigned)p[off+3]<<8)|p[off+4];
     if(!body_raw_valid(id,target) || time!=20 || p[off+5] || p[off+6])return false;}}
 return true;
}
static inline int body_sync_encode(unsigned mask,unsigned reg,const uint8_t data[2][6],uint8_t out[16])
{
 if(!data || !out || !mask || (mask&~3u) || (reg!=40 && reg!=42))return -EINVAL;
 unsigned len=reg==40?1:6,n=2;out[0]=reg;out[1]=len;
 for(unsigned id=1;id<=2;id++)if(mask&(1u<<(id-1)))
  {out[n++]=id;for(unsigned j=0;j<len;j++)out[n++]=data[id-1][j];}
 return body_sync_payload_valid(out,n)?(int)n:-ERANGE;
}
static inline bool body_unicast_payload_valid(unsigned id,const uint8_t *p,int count)
{
 if(id<1 || id>2 || !p || (count!=2 && count!=7))return false;
 uint8_t v[9];unsigned len=count-1;v[0]=p[0];v[1]=len;v[2]=id;
 for(unsigned i=0;i<len;i++)v[i+3]=p[i+1];
 return body_sync_payload_valid(v,len+3);
}
#endif
