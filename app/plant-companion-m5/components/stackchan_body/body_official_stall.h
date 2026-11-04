#ifndef BODY_OFFICIAL_STALL_H
#define BODY_OFFICIAL_STALL_H
#include <stdbool.h>
/* Detection logic translated from the unmodified official ScsServo
 * update_stall_protection. Scheduling and bus decoding stay with caller.
 * A positive detection enters the existing bounded power-cut latch. */
struct body_official_stall {bool valid;int direction,pos,current,load,count;};
static inline int body_stall_abs(int x){return x<0?-x:x;}
static inline void body_official_stall_reset(struct body_official_stall *s)
{s->valid=false;s->direction=0;s->count=0;}
static inline bool body_official_stall_sample(struct body_official_stall *s,int target,int pos,int current,int load)
{
 if(pos<0 || pos>1000){body_official_stall_reset(s);return false;}
 int delta=target-pos;
 if(body_stall_abs(delta)<8){body_official_stall_reset(s);return false;}
 int direction=delta>0?1:-1;current=body_stall_abs(current);load=body_stall_abs(load);
 if(s->valid && direction==s->direction)
  {int movement=body_stall_abs(pos-s->pos);
   bool current_spike=current>=350 || current-s->current>=80;
   bool load_spike=load>=650 || load-s->load>=150;
   if(movement<=1 && (current_spike || load_spike))s->count++;
   else if(movement>1)s->count=0;}
 else s->count=0;
 s->pos=pos;s->current=current;s->load=load;s->direction=direction;s->valid=true;
 return s->count>=2;
}
#endif
