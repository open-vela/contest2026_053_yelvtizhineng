#ifndef PLANT_INTERACTION_POLICY_H
#define PLANT_INTERACTION_POLICY_H
#include <errno.h>
#include "../components/stackchan_body/motion_limits.h"
/* Pixel errors become small relative steps, never a full-range servo command. */
static inline int interaction_follow_target(int cx,int cy,int sx,int sy,
                                           int yaw,int pitch,int *next_yaw,int *next_pitch)
{
  if(!next_yaw||!next_pitch||cx<0||cx>1000||cy<0||cy>1000)return -EINVAL;
  if((sx!=1&&sx!=-1)||(sy!=1&&sy!=-1))return -EAGAIN;
  if(yaw<BODY_YAW_MIN||yaw>BODY_YAW_MAX||pitch<BODY_PITCH_MIN||pitch>BODY_PITCH_MAX)return -ERANGE;
  int y=yaw,p=pitch;
  if(cx>600)y-=2*sx;else if(cx<400)y+=2*sx;
  if(cy>600)p-=sy;else if(cy<400)p+=sy;
  if(y>BODY_YAW_MAX)y=BODY_YAW_MAX;if(y<BODY_YAW_MIN)y=BODY_YAW_MIN;
  if(p>BODY_PITCH_MAX)p=BODY_PITCH_MAX;if(p<BODY_PITCH_MIN)p=BODY_PITCH_MIN;
  *next_yaw=y;*next_pitch=p;
  return y!=yaw||p!=pitch;
}
#endif
