#include "power_policy.h"
void box_power_policy_activity(struct box_power_policy *p,uint32_t now)
{p->last=now;p->initialized=true;p->off_pending=false;p->state=BOX_ACTIVE;}
void box_power_policy_request(struct box_power_policy *p,enum box_power_request r,uint32_t now)
{
 if(r==BOX_POWER_NONE)return;
 box_power_policy_activity(p,now);
 if(r==BOX_POWER_NORMAL){p->eco=false;p->hold=false;}
 if(r==BOX_POWER_ECO){p->eco=true;p->hold=false;}
 if(r==BOX_POWER_OFF){p->off_pending=true;p->hold=false;}
 if(r==BOX_POWER_HOLD){p->hold=true;p->hold_start=now;}
}
enum box_power_state box_power_policy_step(struct box_power_policy *p,uint32_t now,bool busy)
{
 if(!p->initialized)box_power_policy_activity(p,now);
 if(p->hold&&(uint32_t)(now-p->hold_start)>=300000u){p->hold=false;p->last=now;}
 /* Do not clear a requested screen-off while its acknowledgement plays. */
 if(busy){p->last=now;p->state=BOX_ACTIVE;return p->state;}
 if(p->off_pending){p->state=BOX_STANDBY;return p->state;}
 uint32_t idle=now-p->last;
 p->state=p->hold?BOX_ACTIVE:idle>=(p->eco?30000u:60000u)?BOX_STANDBY:
          idle>=(p->eco?15000u:30000u)?BOX_DIM:BOX_ACTIVE;
 return p->state;
}

unsigned box_power_retry_seconds(unsigned failures,bool standby)
{if(!standby)return 10u;if(failures>=5)return 300u;return 10u<<failures;}

/* Local wake monitoring, including unconfirmed sound, must not reset idle. */
unsigned box_power_busy_mask(bool voice,bool hearing,bool confirmed,bool ui,bool ai,bool camera)
{return (voice?1u:0u)|((hearing&&confirmed)?2u:0u)|(ui?4u:0u)|(ai?8u:0u)|(camera?16u:0u);}
