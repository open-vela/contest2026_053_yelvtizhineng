#include "body_feedback_policy.h"
/* Official Hal::ScsServo order: pitch feedback at50ms before WritePos,
 *20ms trajectory updates including equal targets. Each target is written
 * once; missing ACK is counted separately and does not insert queries or
 * a resend. Only the mandatory terminal exact-goal, encoder and health
 * readback can accept a completed pose. Hard UART and sensor errors cut.
 */
static uint32_t g_official_health_at[2];
static bool g_official_health_valid[2];
static unsigned g_official_initial_ACK,g_official_fresh_proof;
static unsigned g_official_transmitted,g_official_unacknowledged;
static unsigned g_official_health_reads,g_official_cached_health;
static unsigned g_official_feedback_unavailable;
static unsigned g_official_unchanged_sent,g_official_final_poses,g_official_final_axes;
void stackchan_body_official_reset(void)
{g_official_health_valid[0]=g_official_health_valid[1]=false;}
int stackchan_body_official_frame(unsigned mask,const uint8_t data[2][6])
{
 if(g_power_state==BODY_FAULTED)return -EHOSTDOWN;
 if(g_power_state!=BODY_READY || !g_servo_ok)return -ENODEV;
 if(!mask || (mask&~3u) || !data)return body_sync_failed(-EINVAL,70,0,0,0);
 if(g_sync_window.active)return body_sync_failed(-EBUSY,70,0,0,0);
 g_sync_window.started=now_ms();g_sync_window.active=true;g_goal_retry_mask=0;
 for(unsigned id=1;id<=2;id++)
   {
    if(!body_official_feedback_axis(id,mask,false,false))continue;
    unsigned slot=id-1;
    if(g_official_health_valid[slot] && (uint32_t)(now_ms()-g_official_health_at[slot])<50)
      {g_official_cached_health++;continue;}
    uint32_t sample_started=now_ms();
    /* Official updates the50ms poll timestamp BEFORE FeedBack and does
     * not reset it when a read is unavailable. No stale health is accepted. */
    g_official_health_at[slot]=sample_started;g_official_health_valid[slot]=true;
    g_hal_feedback_periodic=true;bool healthy=motor_feedback_ok(id);g_hal_feedback_periodic=false;
    if(!healthy && g_feedback_rc[slot]==-ENODATA)
      {g_official_feedback_unavailable++;g_official_health_reads++;g_sync_health_reads++;continue;}
    g_official_health_reads++;g_sync_health_reads++;
    if(!healthy && g_feedback_rc[slot]>=0)
      {usleep(20000);sample_started=now_ms();healthy=motor_feedback_ok(id);
       g_official_health_reads++;g_sync_health_reads++;}
    if(!healthy)
      {g_official_health_valid[slot]=false;
       return body_sync_failed(g_feedback_rc[slot]<0?g_feedback_rc[slot]:-ERANGE,71,id,0,0);}
    g_official_health_at[slot]=sample_started;g_official_health_valid[slot]=true;
   }
 unsigned n=(mask&1u)+((mask>>1)&1u),repeat=0;
 for(unsigned id=1;id<=2;id++)if(mask&(1u<<(id-1)))
   {if(g_servo_reply_level[id-1]!=1)return body_sync_failed(-EPROTO,72,id,1,g_servo_reply_level[id-1]);
    int goal=((unsigned)data[id-1][0]<<8)|data[id-1][1];
    if(g_ramp[id-1].step && goal==g_ramp[id-1].current)repeat++;}
 /* Exact official HAL order: one WritePos per selected axis. A missing
  * write ACK does not insert goal/health probes or an extra absolute resend.
  * It remains an UNCONFIRMED transmission until the mandatory terminal
  * goal+encoder+health readback; never count it as protocol-confirmed. */
 g_goal_staged+=n;
 for(unsigned id=1;id<=2;id++)if(mask&(1u<<(id-1)))
   {
    g_ackprobe_wire_id=id;
    int rc=servo_write(id,42,data[id-1],6);g_ackprobe_wire_id=0;
    if(rc<0)return body_sync_failed(rc,73,id,0,0);
    g_official_transmitted++;
    if(rc==BODY_ACKPROBE_PENDING)g_official_unacknowledged++;
    else{g_official_initial_ACK++;g_goal_applied++;g_goal_write_packets++;}
   }
 g_official_unchanged_sent+=repeat;
 g_sync_window.active=false;
 return 0;
}
/* Numeric RAM evidence only. No extra UART/I2C, allocation or waits. */
static volatile int g_official_final_trace[12];
static int body_official_final_result(int rc,int phase,int id)
{g_official_final_trace[9]=rc;g_official_final_trace[10]=phase;g_official_final_trace[11]=id;return rc;}
int stackchan_body_official_final(unsigned axes,const int target[2])
{
 g_official_final_trace[0]=g_power_state;g_official_final_trace[1]=g_servo_ok;
 g_official_final_trace[2]=axes;g_official_final_trace[3]=target?target[0]:-1;g_official_final_trace[4]=target?target[1]:-1;
 for(unsigned i=5;i<=8;i++)g_official_final_trace[i]=-1;
 if(g_power_state==BODY_FAULTED)return body_official_final_result(-EHOSTDOWN,1,0);
 if(g_power_state!=BODY_READY || !g_servo_ok)return body_official_final_result(-ENODEV,1,0);
 if(!axes || (axes&~3u) || !target)return body_official_final_result(-EINVAL,1,0);
 for(unsigned id=1;id<=2;id++)
   {
    int goal=-1;bool healthy=motor_goal_feedback_ok(id,&goal);
    if(!healthy && g_feedback_rc[id-1]>=0)
      {usleep(20000);healthy=motor_goal_feedback_ok(id,&goal);}
    g_official_final_trace[4+id]=goal;g_official_final_trace[6+id]=g_feedback_position[id-1];
    if(!healthy)return body_official_final_result(g_feedback_rc[id-1]<0?g_feedback_rc[id-1]:-ERANGE,2,id);
    if((axes&(1u<<(id-1))) && goal!=target[id-1])return body_official_final_result(-EIO,3,id);
    if(abs((int)g_feedback_position[id-1]-target[id-1])>12)return body_official_final_result(-ERANGE,4,id);
   }
 g_official_final_poses++;g_official_final_axes+=(axes&1u)+((axes>>1)&1u);
 return body_official_final_result(0,5,0);
}

/* Official normal rest mode releases torque. Goal equality was proved
 * before OFF; OFF-mode goal tracking is not misclassified as a lost WRITE.
 * Keep actual encoder bounds and both full health blocks after release. */
int stackchan_body_official_unload(const int target[2])
{
 if(g_power_state==BODY_FAULTED)return body_official_final_result(-EHOSTDOWN,6,0);
 if(g_power_state!=BODY_READY || !g_servo_ok || !target)return body_official_final_result(-ENODEV,6,0);
 release_servos();
 if(g_power_state==BODY_FAULTED || !g_servo_ok)return body_official_final_result(-EHOSTDOWN,6,0);
 for(unsigned id=1;id<=2;id++)
   {
    bool healthy=motor_feedback_ok(id);
    if(!healthy && g_feedback_rc[id-1]>=0)
      {usleep(20000);healthy=motor_feedback_ok(id);}
    g_official_final_trace[6+id]=g_feedback_position[id-1];
    if(!healthy)return body_official_final_result(g_feedback_rc[id-1]<0?g_feedback_rc[id-1]:-ERANGE,7,id);
    if(abs((int)g_feedback_position[id-1]-target[id-1])>12)return body_official_final_result(-ERANGE,8,id);
   }
 printf("[BodyUnload] torque_OFF_readback=2 post_OFF_health=2 pose_within12=1\n");
 return body_official_final_result(0,9,0);
}
