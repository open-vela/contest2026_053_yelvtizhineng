#ifndef PLANT_STACKCHAN_MOTION_LIMITS_H
#define PLANT_STACKCHAN_MOTION_LIMITS_H
#include <errno.h>
/* This unit's original factory NVS: zero_pos_1=460, zero_pos_2=620.
 * Official mapping is 16 raw ticks / 5 degrees. This assembled unit has
 * passed 5..65 degrees; attempts above this triggered touch interruption.
 * API pitch is relative to a comfortable 20-degree home, not power-on pose. */
#define BODY_YAW_ZERO 460
#define BODY_PITCH_ZERO 620
#define BODY_PITCH_HOME_DEG 20
#define BODY_YAW_MIN (-120)
#define BODY_YAW_MAX 120
#define BODY_PITCH_MIN (-15)
#define BODY_PITCH_MAX 45
#define BODY_HOME_YAW BODY_YAW_ZERO
#define BODY_HOME_PITCH (BODY_PITCH_ZERO + BODY_PITCH_HOME_DEG * 16 / 5)
static inline int body_raw_min(int id)
{return id==1?BODY_YAW_ZERO+BODY_YAW_MIN*16/5:BODY_PITCH_ZERO+5*16/5;}
static inline int body_raw_max(int id)
{return id==1?BODY_YAW_ZERO+BODY_YAW_MAX*16/5:BODY_PITCH_ZERO+65*16/5;}
static inline int body_raw_valid(int id,int raw)
{return (id==1||id==2) && raw>=body_raw_min(id) && raw<=body_raw_max(id);}
/* Official ScsServo::is_raw_pos_valid validates sensor feedback against
 * rawPosLimit(0,1000), independently of command angle limits. */
static inline int body_feedback_raw_valid(int id,int raw)
{return (id==1||id==2) && raw>=0 && raw<=1000;}
static inline int body_feedback_anchor(int id,int raw)
{return raw<body_raw_min(id)?body_raw_min(id):raw>body_raw_max(id)?body_raw_max(id):raw;}
static inline unsigned body_move_ms(int delta)
{unsigned n=delta<0?(unsigned)-delta:(unsigned)delta;unsigned ms=n*1000u/160u;return ms<800?800:ms;}
#endif
